// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL_emfs_internal.h"

#ifdef SDL_PLATFORM_WIIU
#include <new>

// Exported by coreinit; the WUT FS facade does not yet declare this entry point.
extern "C" FSStatus FSOpenFileByStat(FSClient *, FSCmdBlock *, FSStat *, const char *, const char *, FSFileHandle *, FSErrorFlag);
extern "C" void FSCancelCommand(FSClient *, FSCmdBlock *);

namespace
{
constexpr size_t RegionSize = 64 * 1024;
constexpr size_t MaxBounce = 1024 * 1024;
struct Stream;
struct Region
{
    Stream *owner;
    Uint8 *buffer;
    Uint64 offset = SDL_MAX_UINT64;
    Sint32 bytes = 0;
    FSCmdBlock command;
};
struct Stream
{
    FSClient client;
    FSCmdBlock command;
    FSFileHandle handle;
    Uint64 position = 0;
    Uint64 size;
    Region regions[2];
    Region *pending = nullptr;
    SDL_Mutex *lock;
    SDL_Condition *condition;
    Uint8 *cache;
    Uint8 *bounce = nullptr;
    size_t bounce_size = 0;
};

void ReadComplete(FSClient *, FSCmdBlock *, FSStatus result, uint32_t param)
{
    auto *region = reinterpret_cast<Region *>(param);
    Stream *stream = region->owner;
    SDL_LockMutex(stream->lock);
    region->bytes = result == FS_STATUS_END ? 0 : result;
    stream->pending = nullptr;
    SDL_BroadcastCondition(stream->condition);
    SDL_UnlockMutex(stream->lock);
}

bool ResizeBounce(Stream *s, size_t size)
{
    if (size <= s->bounce_size)
        return true;
    Uint8 *buffer = static_cast<Uint8 *>(SDL_aligned_alloc(64, size));
    if (!buffer)
        return false;
    SDL_aligned_free(s->bounce);
    s->bounce = buffer;
    s->bounce_size = size;
    return true;
}

size_t Transfer(Stream *s, void *destination, Uint64 offset, size_t length, SDL_IOStatus *status)
{
    size_t total = 0;
    auto *out = static_cast<Uint8 *>(destination);
    while (total < length)
    {
        const size_t remaining = length - total;
        const bool aligned = (reinterpret_cast<uintptr_t>(out + total) & 63) == 0 && remaining >= 64;
        const size_t amount = aligned ? SDL_min(remaining & ~size_t(63), size_t(SDL_MAX_SINT32 & ~63)) : SDL_min(remaining, MaxBounce);
        if (!aligned && !ResizeBounce(s, amount))
        {
            *status = SDL_IO_STATUS_ERROR;
            break;
        }
        Uint8 *buffer = aligned ? out + total : s->bounce;
        FSStatus result = FSReadFileWithPos(&s->client, &s->command, buffer, 1, amount, offset + total, s->handle, 0, FS_ERROR_FLAG_ALL);
        if (result == FS_STATUS_END || result == 0)
            break;
        if (result < 0)
        {
            SDL_SetError("Wii U asset read failed: %d", result);
            *status = SDL_IO_STATUS_ERROR;
            break;
        }
        if (!aligned)
            SDL_memcpy(out + total, buffer, result);
        total += result;
        if (size_t(result) < amount)
            break;
    }
    return total;
}

Sint64 SDLCALL Size(void *data)
{
    return static_cast<Stream *>(data)->size;
}
Sint64 SDLCALL Seek(void *data, Sint64 offset, SDL_IOWhence whence)
{
    auto *s = static_cast<Stream *>(data);
    Sint64 base;
    switch (whence)
    {
    case SDL_IO_SEEK_SET:
        base = 0;
        break;
    case SDL_IO_SEEK_CUR:
        base = s->position;
        break;
    case SDL_IO_SEEK_END:
        base = s->size;
        break;
    default:
        SDL_SetError("Invalid asset seek origin");
        return -1;
    }
    if (offset < -base || offset > SDL_MAX_UINT32 - base)
    {
        SDL_SetError("Asset seek out of range");
        return -1;
    }
    s->position = base + offset;
    return s->position;
}

size_t SDLCALL Read(void *data, void *destination, size_t length, SDL_IOStatus *status)
{
    auto *s = static_cast<Stream *>(data);
    auto *out = static_cast<Uint8 *>(destination);
    *status = SDL_IO_STATUS_READY;
    const size_t wanted = SDL_min(Uint64(length), s->position < s->size ? s->size - s->position : 0);
    size_t total = 0;
    while (total < wanted)
    {
        const Uint64 offset = s->position & ~Uint64(RegionSize - 1);
        Region *region = nullptr;
        SDL_LockMutex(s->lock);
        while (s->pending && s->pending->offset == offset)
            SDL_WaitCondition(s->condition, s->lock);
        for (auto &r : s->regions)
            if (&r != s->pending && r.offset == offset)
                region = &r;
        SDL_UnlockMutex(s->lock);
        if (!region && wanted - total >= RegionSize)
        {
            // Consume any already queued region instead of issuing overlapping I/O.
            size_t amount = wanted - total;
            for (auto &r : s->regions)
                if (r.offset > s->position && r.offset != SDL_MAX_UINT64)
                    amount = SDL_min(Uint64(amount), r.offset - s->position);
            size_t got = Transfer(s, out + total, s->position, amount, status);
            s->position += got;
            total += got;
            if (got < amount)
                break;
            continue;
        }
        if (!region)
        {
            SDL_LockMutex(s->lock);
            region = s->pending == &s->regions[0] ? &s->regions[1] : &s->regions[0];
            SDL_UnlockMutex(s->lock);
            region->offset = offset;
            region->bytes = Transfer(s, region->buffer, offset, RegionSize, status);
            if (*status == SDL_IO_STATUS_ERROR)
            {
                region->offset = SDL_MAX_UINT64;
                break;
            }
        }
        if (region->bytes < 0)
        {
            SDL_SetError("Wii U asset prefetch failed: %d", region->bytes);
            *status = SDL_IO_STATUS_ERROR;
            break;
        }
        size_t start = s->position - offset;
        if (start >= size_t(region->bytes))
            break;
        size_t amount = SDL_min(wanted - total, size_t(region->bytes) - start);
        SDL_memcpy(out + total, region->buffer + start, amount);
        s->position += amount;
        total += amount;
        SDL_LockMutex(s->lock);
        Uint64 next = offset + RegionSize;
        Region *other = region == &s->regions[0] ? &s->regions[1] : &s->regions[0];
        if (!s->pending && next < s->size && other->offset != next)
        {
            other->offset = next;
            other->bytes = 0;
            FSInitCmdBlock(&other->command);
            FSAsyncData async{ReadComplete, reinterpret_cast<uint32_t>(other), nullptr};
            s->pending = other;
            FSStatus result = FSReadFileWithPosAsync(&s->client, &other->command, other->buffer, 1, RegionSize, next, s->handle, 0,
                                                     FS_ERROR_FLAG_ALL, &async);
            if (result != FS_STATUS_OK)
            {
                s->pending = nullptr;
                other->offset = SDL_MAX_UINT64;
            }
        }
        SDL_UnlockMutex(s->lock);
    }
    if (total < length && *status != SDL_IO_STATUS_ERROR)
        *status = SDL_IO_STATUS_EOF;
    return total;
}

bool SDLCALL Close(void *data)
{
    auto *s = static_cast<Stream *>(data);
    SDL_LockMutex(s->lock);
    if (s->pending)
        FSCancelCommand(&s->client, &s->pending->command);
    while (s->pending)
        SDL_WaitCondition(s->condition, s->lock);
    SDL_UnlockMutex(s->lock);
    FSStatus result = FSCloseFile(&s->client, &s->command, s->handle, FS_ERROR_FLAG_ALL);
    FSDelClient(&s->client, FS_ERROR_FLAG_ALL);
    SDL_aligned_free(s->bounce);
    SDL_aligned_free(s->cache);
    SDL_DestroyCondition(s->condition);
    SDL_DestroyMutex(s->lock);
    delete s;
    return result == FS_STATUS_OK || SDL_SetError("Wii U asset close failed: %d", result);
}

Stream *OpenNativeAsset(const SDL_EMFS_AssetEntry &entry)
{
    // Native open-by-stat consumes the filesystem cookie in the reserved tail.
    // Synthesized metadata (for example Aroma RomFS) has no native cookie.
    const auto *raw = reinterpret_cast<const Uint8 *>(&entry.native_stat);
    constexpr size_t cookie = offsetof(FSStat, modified) + sizeof(FSStat::modified);
    bool native = false;
    for (size_t i = cookie; i < sizeof(FSStat); ++i)
        native |= raw[i] != 0;
    if (!native)
        return nullptr;
    auto *s = new (std::nothrow) Stream{};
    if (!s)
    {
        SDL_OutOfMemory();
        return nullptr;
    }
    FSInit();
    FSStatus result = FSAddClient(&s->client, FS_ERROR_FLAG_ALL);
    if (result != FS_STATUS_OK)
    {
        delete s;
        SDL_SetError("FSAddClient failed: %d", result);
        return nullptr;
    }
    FSInitCmdBlock(&s->command);
    FSStat stat = entry.native_stat;
    result = FSOpenFileByStat(&s->client, &s->command, &stat, "r", entry.path.c_str(), &s->handle, FS_ERROR_FLAG_ALL);
    if (result != FS_STATUS_OK)
    {
        FSDelClient(&s->client, FS_ERROR_FLAG_ALL);
        delete s;
        return nullptr;
    }
    s->size = entry.info.size;
    s->lock = SDL_CreateMutex();
    s->condition = SDL_CreateCondition();
    s->cache = static_cast<Uint8 *>(SDL_aligned_alloc(64, 2 * RegionSize));
    if (!s->lock || !s->condition || !s->cache)
    {
        Close(s);
        return nullptr;
    }
    for (int i = 0; i < 2; ++i)
    {
        s->regions[i].owner = s;
        s->regions[i].buffer = s->cache + i * RegionSize;
    }
    return s;
}
} // namespace

SDL_IOStream *SDL_EMFS_WiiUOpenAsset(const SDL_EMFS_AssetEntry &entry)
{
    Stream *s = OpenNativeAsset(entry);
    if (!s)
        return nullptr;
    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = Size;
    iface.seek = Seek;
    iface.read = Read;
    iface.close = Close;
    SDL_IOStream *stream = SDL_OpenIO(&iface, s);
    if (!stream)
        Close(s);
    return stream;
}

void SDL_EMFS_QueueCompletion(SDL_EMFS_Context *context, SDL_EMFS_AsyncOperation *operation)
{
    SDL_LockMutex(context->completion_lock);
    context->completions.push_back(operation);
    SDL_UnlockMutex(context->completion_lock);
}

void *SDL_EMFS_WiiUOpenAsyncAsset(const SDL_EMFS_AssetEntry &entry)
{
    return OpenNativeAsset(entry);
}

bool SDL_EMFS_WiiUCloseAsyncAsset(void *asset)
{
    return Close(asset);
}

namespace
{
struct AsyncRead
{
    SDL_EMFS_AsyncOperation *operation;
    Stream *stream;
    Uint64 offset;
    size_t amount = 0;
    bool bounced = false;
};

void Finish(AsyncRead *read)
{
    auto *op = read->operation;
    if (op->type == SDL_EMFS_OperationType::Load)
    {
        if (op->result == SDL_ASYNCIO_COMPLETE)
        {
            op->size = op->bytes_transferred;
            static_cast<Uint8 *>(op->buffer)[op->size] = 0;
        }
        else
        {
            SDL_free(op->buffer);
            op->buffer = nullptr;
            op->size = 0;
        }
    }
    SDL_EMFS_QueueCompletion(op->context, op);
    delete read;
}

void Submit(AsyncRead *read);
void AsyncComplete(FSClient *, FSCmdBlock *, FSStatus result, uint32_t param)
{
    auto *read = reinterpret_cast<AsyncRead *>(param);
    auto *op = read->operation;
    if (result < 0 && result != FS_STATUS_END)
    {
        op->result = SDL_ASYNCIO_FAILURE;
        op->error = "Wii U asynchronous asset read failed: " + std::to_string(result);
        Finish(read);
        return;
    }
    if (result > 0)
    {
        if (read->bounced)
            SDL_memcpy(static_cast<Uint8 *>(op->buffer) + op->bytes_transferred, read->stream->bounce, result);
        op->bytes_transferred += result;
    }
    if (result <= 0 || size_t(result) < read->amount)
        Finish(read);
    else
        Submit(read);
}

void Submit(AsyncRead *read)
{
    auto *op = read->operation;
    auto *s = read->stream;
    Uint64 position = read->offset + op->bytes_transferred;
    size_t left = SDL_min(Uint64(op->size) - op->bytes_transferred, position < s->size ? s->size - position : 0);
    if (!left)
    {
        Finish(read);
        return;
    }
    auto *out = static_cast<Uint8 *>(op->buffer) + op->bytes_transferred;
    read->bounced = (reinterpret_cast<uintptr_t>(out) & 63) != 0 || left < 64;
    read->amount = read->bounced ? SDL_min(left, MaxBounce) : SDL_min(left & ~size_t(63), size_t(SDL_MAX_SINT32 & ~63));
    if (read->bounced && !ResizeBounce(s, read->amount))
    {
        op->result = SDL_ASYNCIO_FAILURE;
        op->error = SDL_GetError();
        Finish(read);
        return;
    }
    FSInitCmdBlock(&s->command);
    FSAsyncData async{AsyncComplete, reinterpret_cast<uint32_t>(read), nullptr};
    FSStatus result = FSReadFileWithPosAsync(&s->client, &s->command, read->bounced ? s->bounce : out, 1, read->amount, position, s->handle,
                                             0, FS_ERROR_FLAG_ALL, &async);
    if (result != FS_STATUS_OK)
    {
        op->result = SDL_ASYNCIO_FAILURE;
        op->error = "Wii U asynchronous asset submission failed: " + std::to_string(result);
        Finish(read);
    }
}
} // namespace

void SDL_EMFS_WiiUReadAsync(SDL_EMFS_AsyncOperation *operation, Uint64 offset)
{
    auto *read = new (std::nothrow) AsyncRead{operation, static_cast<Stream *>(operation->file->wiiu_asset), offset};
    if (!read)
    {
        operation->result = SDL_ASYNCIO_FAILURE;
        operation->error = "Out of memory";
        if (operation->type == SDL_EMFS_OperationType::Load)
        {
            Close(operation->file->wiiu_asset);
            delete operation->file;
            operation->file = nullptr;
            SDL_free(operation->buffer);
            operation->buffer = nullptr;
            operation->size = 0;
        }
        SDL_EMFS_QueueCompletion(operation->context, operation);
        return;
    }
    Submit(read);
}

bool SDL_EMFS_WiiULoadAsync(SDL_EMFS_AsyncOperation *operation, const SDL_EMFS_AssetEntry &entry)
{
    Stream *stream = OpenNativeAsset(entry);
    if (!stream)
        return false;
    auto *file = new (std::nothrow) SDL_EMFS_AsyncFile;
    operation->size = entry.info.size;
    operation->buffer = operation->size == size_t(-1) ? nullptr : SDL_malloc(operation->size + 1);
    if (!file || !operation->buffer)
    {
        delete file;
        SDL_free(operation->buffer);
        operation->buffer = nullptr;
        operation->size = 0;
        Close(stream);
        operation->result = SDL_ASYNCIO_FAILURE;
        operation->error = "Out of memory";
        SDL_EMFS_QueueCompletion(operation->context, operation);
        return true;
    }
    file->context = operation->context;
    file->wiiu_asset = stream;
    operation->file = file;
    SDL_EMFS_WiiUReadAsync(operation, 0);
    return true;
}
#endif
