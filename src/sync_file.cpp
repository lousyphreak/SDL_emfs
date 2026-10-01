// SPDX-License-Identifier: GPL-3.0-or-later

#include "SDL_emfs_internal.h"

#include <new>

namespace {

SDL_EMFS_File *OpenAsset(SDL_EMFS_Context *context, const std::string &path, const char *mode)
{
    if (!SDL_EMFS_IsAssetMode(mode)) {
        SDL_SetError("Asset files are read-only");
        return nullptr;
    }
    const auto found = context->assets.find(path);
    if (found == context->assets.end()) {
        SDL_SetError("Asset not found: %s", path.c_str());
        return nullptr;
    }

    SDL_EMFS_File *file = new (std::nothrow) SDL_EMFS_File;
    if (!file) {
        SDL_OutOfMemory();
        return nullptr;
    }
    file->context = context;
    file->path = path;
    file->etag = found->second.etag;
    file->size = found->second.info.size;
    file->asset = true;
#ifndef __EMSCRIPTEN__
    file->stream = SDL_IOFromFile(found->second.path.c_str(), mode);
    if (!file->stream) {
        delete file;
        return nullptr;
    }
#endif
    return file;
}

bool MaterializeAsset(SDL_EMFS_Context *context, const std::string &path, const std::string &physical_path)
{
    const auto asset = context->assets.find(path);
    if (asset == context->assets.end()) {
        return false;
    }
    size_t size = 0;
    void *data = nullptr;
#ifdef __EMSCRIPTEN__
    data = SDL_EMFS_WebLoad(context, asset->second, &size);
#else
    data = SDL_LoadFile(asset->second.path.c_str(), &size);
#endif
    if (!data) {
        return false;
    }
    const bool success = SDL_EMFS_CreateParentDirectory(context, path) && SDL_SaveFile(physical_path.c_str(), data, size);
    SDL_free(data);
    return success;
}

#ifdef __EMSCRIPTEN__

Sint64 SDLCALL IOSize(void *userdata)
{
    return SDL_EMFS_GetSize(static_cast<SDL_EMFS_File *>(userdata));
}

Sint64 SDLCALL IOSeek(void *userdata, Sint64 offset, SDL_IOWhence whence)
{
    return SDL_EMFS_Seek(static_cast<SDL_EMFS_File *>(userdata), offset, whence);
}

size_t SDLCALL IORead(void *userdata, void *destination, size_t size, SDL_IOStatus *status)
{
    SDL_EMFS_File *file = static_cast<SDL_EMFS_File *>(userdata);
    const size_t result = SDL_EMFS_Read(file, destination, size);
    if (result != size) {
        *status = file->status;
    }
    return result;
}

size_t SDLCALL IOWrite(void *userdata, const void *source, size_t size, SDL_IOStatus *status)
{
    SDL_EMFS_File *file = static_cast<SDL_EMFS_File *>(userdata);
    const size_t result = SDL_EMFS_Write(file, source, size);
    if (result != size) {
        *status = file->status;
    }
    return result;
}

bool SDLCALL IOFlush(void *userdata, SDL_IOStatus *status)
{
    SDL_EMFS_File *file = static_cast<SDL_EMFS_File *>(userdata);
    const bool result = SDL_EMFS_Flush(file);
    if (!result) {
        *status = file->status;
    }
    return result;
}

bool SDLCALL IOClose(void *userdata)
{
    return SDL_EMFS_Close(static_cast<SDL_EMFS_File *>(userdata));
}

SDL_IOStream *OpenCustomIO(SDL_EMFS_File *file)
{
    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = IOSize;
    iface.seek = IOSeek;
    iface.read = IORead;
    iface.write = IOWrite;
    iface.flush = IOFlush;
    iface.close = IOClose;
    SDL_IOStream *stream = SDL_OpenIO(&iface, file);
    if (!stream) {
        SDL_EMFS_Close(file);
    }
    return stream;
}

#endif

} // namespace

SDL_EMFS_File *SDL_EMFS_OpenResolved(SDL_EMFS_Context *context, SDL_EMFS_Root root, const std::string &path, const char *mode)
{
    bool asset = false;
    std::string physical_path;
    if (!SDL_EMFS_PrepareOpen(context, root, path, mode, asset, physical_path)) {
        return nullptr;
    }
    if (asset) {
        return OpenAsset(context, path, mode);
    }

    SDL_IOStream *stream = SDL_IOFromFile(physical_path.c_str(), mode);
    if (!stream) {
        return nullptr;
    }
    SDL_EMFS_File *file = new (std::nothrow) SDL_EMFS_File;
    if (!file) {
        SDL_CloseIO(stream);
        SDL_OutOfMemory();
        return nullptr;
    }
    file->context = context;
    file->stream = stream;
    file->path = path;
    file->writable = SDL_EMFS_IsWritableMode(mode);
    const Sint64 size = SDL_GetIOSize(stream);
    file->size = size >= 0 ? (Uint64)size : 0;
    return file;
}

bool SDL_EMFS_PrepareOpen(SDL_EMFS_Context *context, SDL_EMFS_Root root, const std::string &path, const char *mode, bool &asset, std::string &physical_path)
{
    if (!mode || !*mode) {
        return SDL_InvalidParamError("mode");
    }
    if (root == SDL_EMFS_ROOT_ASSET) {
        if (!SDL_EMFS_IsAssetMode(mode)) {
            return SDL_SetError("Asset files are read-only");
        }
        if (!SDL_EMFS_ResolveAsset(context, path, physical_path)) {
            return false;
        }
        asset = true;
        return true;
    }
    if (root != SDL_EMFS_ROOT_USER) {
        return SDL_SetError("Invalid SDL_emfs root");
    }

    const bool exists = SDL_EMFS_UserPathExists(context, path, physical_path);
    if (!exists && SDL_EMFS_IsReadOnlyMode(mode)) {
        if (!SDL_EMFS_IsAssetMode(mode) || !SDL_EMFS_ResolveAsset(context, path, physical_path)) {
            return false;
        }
        asset = true;
        return true;
    }
    if (!exists && SDL_EMFS_IsPreservingMode(mode)) {
        const bool has_asset = context->assets.find(path) != context->assets.end();
        if (has_asset && !MaterializeAsset(context, path, physical_path)) {
            return false;
        }
    }
    if (SDL_EMFS_IsWritableMode(mode) && !SDL_EMFS_CreateParentDirectory(context, path)) {
        return false;
    }
    asset = false;
    return true;
}

SDL_EMFS_File *SDL_EMFS_Open(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, const char *mode)
{
    SDL_EMFS_ASSERT_READY(context);
    if (!context) {
        SDL_InvalidParamError("context");
        return nullptr;
    }
    std::string normalized;
    if (!SDL_EMFS_NormalizePath(path, normalized)) {
        return nullptr;
    }
    return SDL_EMFS_OpenResolved(context, root, normalized, mode);
}

SDL_IOStream *SDL_EMFS_OpenIO(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, const char *mode)
{
    SDL_EMFS_File *file = SDL_EMFS_Open(context, root, path, mode);
    if (!file) {
        return nullptr;
    }
#ifdef __EMSCRIPTEN__
    if (file->asset || file->writable) {
        return OpenCustomIO(file);
    }
#endif
    SDL_IOStream *stream = file->stream;
    file->stream = nullptr;
    delete file;
    return stream;
}

Sint64 SDL_EMFS_GetSize(SDL_EMFS_File *file)
{
    if (!file) {
        SDL_InvalidParamError("file");
        return -1;
    }
#ifndef __EMSCRIPTEN__
    return SDL_GetIOSize(file->stream);
#else
    return file->asset ? (Sint64)file->size : SDL_GetIOSize(file->stream);
#endif
}

Sint64 SDL_EMFS_Tell(SDL_EMFS_File *file)
{
    if (!file) {
        SDL_InvalidParamError("file");
        return -1;
    }
#ifndef __EMSCRIPTEN__
    return SDL_TellIO(file->stream);
#else
    return file->asset ? (Sint64)file->position : SDL_TellIO(file->stream);
#endif
}

Sint64 SDL_EMFS_Seek(SDL_EMFS_File *file, Sint64 offset, SDL_IOWhence whence)
{
    if (!file) {
        SDL_InvalidParamError("file");
        return -1;
    }
    if (!file->asset) {
        const Sint64 result = SDL_SeekIO(file->stream, offset, whence);
        file->status = SDL_GetIOStatus(file->stream);
        return result;
    }
#ifndef __EMSCRIPTEN__
    const Sint64 result = SDL_SeekIO(file->stream, offset, whence);
    file->status = SDL_GetIOStatus(file->stream);
    return result;
#else

    Sint64 base = 0;
    if (whence == SDL_IO_SEEK_CUR) {
        base = (Sint64)file->position;
    } else if (whence == SDL_IO_SEEK_END) {
        base = (Sint64)file->size;
    } else if (whence != SDL_IO_SEEK_SET) {
        file->status = SDL_IO_STATUS_ERROR;
        SDL_SetError("Invalid seek origin");
        return -1;
    }
    if (offset < -base || (offset > 0 && offset > INT64_MAX - base)) {
        file->status = SDL_IO_STATUS_ERROR;
        SDL_SetError("Invalid seek offset");
        return -1;
    }
    file->position = (Uint64)(base + offset);
    file->status = SDL_IO_STATUS_READY;
    return (Sint64)file->position;
#endif
}

SDL_IOStatus SDL_EMFS_GetStatus(SDL_EMFS_File *file)
{
    if (!file) {
        SDL_InvalidParamError("file");
        return SDL_IO_STATUS_ERROR;
    }
    return file->status;
}

size_t SDL_EMFS_Read(SDL_EMFS_File *file, void *destination, size_t length)
{
    if (!file || (!destination && length)) {
        SDL_InvalidParamError(!file ? "file" : "destination");
        return 0;
    }
    if (!length) {
        return 0;
    }
    if (!file->asset) {
        const size_t result = SDL_ReadIO(file->stream, destination, length);
        file->status = SDL_GetIOStatus(file->stream);
        return result;
    }

#ifdef __EMSCRIPTEN__
    size_t transferred = 0;
    if (!SDL_EMFS_WebRead(file, file->position, destination, length, &transferred)) {
        file->status = SDL_IO_STATUS_ERROR;
        return 0;
    }
    file->position += transferred;
    file->status = transferred < length ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_READY;
    return transferred;
#else
    const size_t result = SDL_ReadIO(file->stream, destination, length);
    file->status = SDL_GetIOStatus(file->stream);
    return result;
#endif
}

size_t SDL_EMFS_Write(SDL_EMFS_File *file, const void *source, size_t length)
{
    if (!file || (!source && length)) {
        SDL_InvalidParamError(!file ? "file" : "source");
        return 0;
    }
    if (file->asset) {
        file->status = SDL_IO_STATUS_READONLY;
        SDL_SetError("Asset files are read-only");
        return 0;
    }
    const size_t result = SDL_WriteIO(file->stream, source, length);
    file->status = SDL_GetIOStatus(file->stream);
    return result;
}

size_t SDL_EMFS_ReadAt(SDL_EMFS_File *file, Uint64 offset, void *destination, size_t length, SDL_IOStatus *status)
{
    if (!file || !status || (!destination && length)) {
        SDL_InvalidParamError(!file ? "file" : (!status ? "status" : "destination"));
        return 0;
    }
    if (file->asset) {
        size_t transferred = 0;
#ifdef __EMSCRIPTEN__
        if (!SDL_EMFS_WebRead(file, offset, destination, length, &transferred)) {
            *status = SDL_IO_STATUS_ERROR;
            return 0;
        }
#else
        const Sint64 saved = SDL_TellIO(file->stream);
        if (saved < 0 || SDL_SeekIO(file->stream, (Sint64)offset, SDL_IO_SEEK_SET) < 0) {
            *status = SDL_IO_STATUS_ERROR;
            return 0;
        }
        transferred = SDL_ReadIO(file->stream, destination, length);
        const SDL_IOStatus read_status = SDL_GetIOStatus(file->stream);
        if (SDL_SeekIO(file->stream, saved, SDL_IO_SEEK_SET) < 0) {
            *status = SDL_IO_STATUS_ERROR;
            return transferred;
        }
        *status = read_status;
        return transferred;
#endif
        *status = transferred < length ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_READY;
        return transferred;
    }

    const Sint64 saved = SDL_TellIO(file->stream);
    if (saved < 0 || offset > INT64_MAX || SDL_SeekIO(file->stream, (Sint64)offset, SDL_IO_SEEK_SET) < 0) {
        *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
    const size_t transferred = SDL_ReadIO(file->stream, destination, length);
    const SDL_IOStatus read_status = SDL_GetIOStatus(file->stream);
    if (SDL_SeekIO(file->stream, saved, SDL_IO_SEEK_SET) < 0) {
        *status = SDL_IO_STATUS_ERROR;
        return transferred;
    }
    *status = read_status;
    return transferred;
}

bool SDL_EMFS_Flush(SDL_EMFS_File *file)
{
    if (!file) {
        return SDL_InvalidParamError("file");
    }
    if (file->asset) {
        return true;
    }
    if (!SDL_FlushIO(file->stream)) {
        file->status = SDL_IO_STATUS_ERROR;
        return false;
    }
    file->status = SDL_GetIOStatus(file->stream);
    if (file->writable && !SDL_EMFS_SyncUserStorage()) {
        file->status = SDL_IO_STATUS_ERROR;
        return false;
    }
    return true;
}

bool SDL_EMFS_Close(SDL_EMFS_File *file)
{
    if (!file) {
        return SDL_InvalidParamError("file");
    }
    bool success = true;
    if (file->stream) {
        success = SDL_CloseIO(file->stream);
    }
    if (file->writable && !SDL_EMFS_SyncUserStorage()) {
        success = false;
    }
    delete file;
    return success;
}

void SDL_EMFS_Prefetch(SDL_EMFS_File *file, Uint64 offset, Uint64 length)
{
#ifdef __EMSCRIPTEN__
    if (file && file->asset && length) {
        SDL_EMFS_WebPrefetch(file, offset, length);
    }
#else
    (void)file;
    (void)offset;
    (void)length;
#endif
}

void *SDL_EMFS_LoadFile(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, size_t *size)
{
    SDL_EMFS_ASSERT_READY(context);
    if (!context) {
        SDL_InvalidParamError("context");
        return nullptr;
    }
    std::string normalized;
    if (!SDL_EMFS_NormalizePath(path, normalized)) {
        return nullptr;
    }

    if (root == SDL_EMFS_ROOT_USER) {
        std::string physical;
        if (SDL_EMFS_UserPathExists(context, normalized, physical)) {
            return SDL_LoadFile(physical.c_str(), size);
        }
        root = SDL_EMFS_ROOT_ASSET;
    }
    if (root != SDL_EMFS_ROOT_ASSET) {
        SDL_SetError("Invalid SDL_emfs root");
        return nullptr;
    }
    const auto found = context->assets.find(normalized);
    if (found == context->assets.end()) {
        SDL_SetError("Asset not found: %s", normalized.c_str());
        return nullptr;
    }
#ifdef __EMSCRIPTEN__
    return SDL_EMFS_WebLoad(context, found->second, size);
#else
    return SDL_LoadFile(found->second.path.c_str(), size);
#endif
}
