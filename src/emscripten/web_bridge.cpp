// SPDX-License-Identifier: GPL-3.0-or-later

#include "../SDL_emfs_internal.h"

#include <emscripten.h>

#include <climits>

extern "C" {

extern void SDL_EMFS_JS_Init(SDL_EMFS_Context *context, const char *asset_root, const char *user_root, const char *application_namespace, Uint32 chunk_size, Uint32 prefetch_chunks, Uint32 memory_cache_chunks);
extern int SDL_EMFS_JS_WaitReady(SDL_EMFS_Context *context);
extern int SDL_EMFS_JS_Read(SDL_EMFS_File *file, const char *path, const char *etag, Uint32 size_low, Uint32 size_high, Uint32 offset_low, Uint32 offset_high, void *destination, Uint32 length);
extern void SDL_EMFS_JS_ReadAsync(SDL_EMFS_AsyncOperation *operation, const char *path, const char *etag, Uint32 size_low, Uint32 size_high, Uint32 offset_low, Uint32 offset_high, void *destination, Uint32 length_low, Uint32 length_high);
extern void SDL_EMFS_JS_Prefetch(const char *path, const char *etag, Uint32 size_low, Uint32 size_high, Uint32 offset_low, Uint32 offset_high, Uint32 length_low, Uint32 length_high);
extern int SDL_EMFS_JS_Load(const char *path, const char *etag, Uint32 size_low, Uint32 size_high, void **buffer, size_t *size);
extern void SDL_EMFS_JS_LoadAsync(SDL_EMFS_AsyncOperation *operation, const char *path, const char *etag, Uint32 size_low, Uint32 size_high);
extern int SDL_EMFS_JS_Sync(void);
extern void SDL_EMFS_JS_SyncAsync(SDL_EMFS_AsyncOperation *operation);

} // extern "C"

namespace {

Uint32 Low(Uint64 value)
{
    return (Uint32)value;
}

Uint32 High(Uint64 value)
{
    return (Uint32)(value >> 32);
}

void SetAsyncError(SDL_EMFS_AsyncOperation *operation, const char *error)
{
    operation->error = (error && *error) ? error : "SDL_emfs web operation failed";
}

} // namespace

void SDL_EMFS_QueueCompletion(SDL_EMFS_Context *context, SDL_EMFS_AsyncOperation *operation)
{
    SDL_LockMutex(context->completion_lock);
    context->completions.emplace_back(operation);
    SDL_UnlockMutex(context->completion_lock);
}

extern "C" EMSCRIPTEN_KEEPALIVE int SDL_EMFS_WebInitializationComplete(SDL_EMFS_Context *context, int success, void *manifest, size_t manifest_size, char *error)
{
    bool ready = success != 0;
    if (ready) {
        ready = SDL_EMFS_ParseManifest(context, manifest, manifest_size) && SDL_EMFS_BuildUserIndex(context);
    }
    if (!ready) {
        context->initialization_error = (error && *error) ? error : SDL_GetError();
        if (context->initialization_error.empty()) {
            context->initialization_error = "SDL_emfs web initialization failed";
        }
    }
    SDL_free(manifest);
    SDL_free(error);
    std::vector<SDL_EMFS_AsyncOperation *> readiness_waiters;
    SDL_LockMutex(context->completion_lock);
    context->ready = ready;
    context->initialization_complete = true;
    readiness_waiters.swap(context->readiness_waiters);
    SDL_UnlockMutex(context->completion_lock);

    for (SDL_EMFS_AsyncOperation *operation : readiness_waiters) {
        operation->success = ready;
        if (!ready) {
            operation->error = context->initialization_error;
        }
        SDL_EMFS_QueueCompletion(context, operation);
    }
    return ready ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE void SDL_EMFS_WebTransferComplete(SDL_EMFS_AsyncOperation *operation, int success, Uint32 bytes_low, Uint32 bytes_high, char *error)
{
    operation->bytes_transferred = (Uint64)bytes_low | ((Uint64)bytes_high << 32);
    operation->result = success ? SDL_ASYNCIO_COMPLETE : SDL_ASYNCIO_FAILURE;
    operation->success = success != 0;
    if (!success) {
        SetAsyncError(operation, error);
    }
    SDL_free(error);
    SDL_EMFS_QueueCompletion(operation->context, operation);
}

extern "C" EMSCRIPTEN_KEEPALIVE void SDL_EMFS_WebLoadComplete(SDL_EMFS_AsyncOperation *operation, void *buffer, size_t size, char *error)
{
    operation->buffer = buffer;
    operation->size = buffer ? size : 0;
    operation->result = buffer ? SDL_ASYNCIO_COMPLETE : SDL_ASYNCIO_FAILURE;
    operation->success = buffer != nullptr;
    if (!buffer) {
        SetAsyncError(operation, error);
    }
    SDL_free(error);
    SDL_EMFS_QueueCompletion(operation->context, operation);
}

extern "C" EMSCRIPTEN_KEEPALIVE void SDL_EMFS_WebSyncComplete(SDL_EMFS_AsyncOperation *operation, int success, char *error)
{
    if (!success) {
        operation->success = false;
        SetAsyncError(operation, error);
    }
    SDL_free(error);
    SDL_EMFS_QueueCompletion(operation->context, operation);
}

extern "C" EMSCRIPTEN_KEEPALIVE void SDL_EMFS_WebSetError(char *error)
{
    SDL_SetError("%s", error && *error ? error : "SDL_emfs web operation failed");
    SDL_free(error);
}

void SDL_EMFS_WebStartInitialization(SDL_EMFS_Context *context)
{
    std::string application_namespace = context->organization;
    application_namespace += application_namespace.empty() ? "" : "/";
    application_namespace += context->application;
    SDL_EMFS_JS_Init(context, context->asset_root.c_str(), context->user_root.c_str(), application_namespace.c_str(), (Uint32)context->chunk_size, context->prefetch_chunks, context->memory_cache_chunks);
}

bool SDL_EMFS_WebWaitReady(SDL_EMFS_Context *context)
{
    return SDL_EMFS_JS_WaitReady(context) != 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE void SDL_EMFS_WebAssetChanged(SDL_EMFS_File *file, Uint32 size_low, Uint32 size_high, char *etag)
{
    file->size = (Uint64)size_low | ((Uint64)size_high << 32);
    file->etag = etag;
    SDL_free(etag);
}

extern "C" EMSCRIPTEN_KEEPALIVE void SDL_EMFS_WebAsyncAssetChanged(SDL_EMFS_AsyncOperation *operation, Uint32 size_low, Uint32 size_high, char *etag)
{
    SDL_EMFS_AsyncFile *file = operation->file;
    file->size = (Uint64)size_low | ((Uint64)size_high << 32);
    file->etag = etag;
    SDL_free(etag);
}

bool SDL_EMFS_WebRead(SDL_EMFS_File *file, Uint64 offset, void *destination, size_t length, size_t *transferred)
{
    if (offset >= file->size) {
        *transferred = 0;
        return true;
    }
    const Uint64 available = file->size - offset;
    const Uint32 clamped = (Uint32)SDL_min((Uint64)SDL_min(length, (size_t)INT_MAX), available);
    const int copied = SDL_EMFS_JS_Read(file, file->path.c_str(), file->etag.c_str(), Low(file->size), High(file->size), Low(offset), High(offset), destination, clamped);
    if (copied < 0) {
        return false;
    }
    *transferred = (size_t)copied;
    return true;
}

void SDL_EMFS_WebPrefetch(SDL_EMFS_File *file, Uint64 offset, Uint64 length)
{
    SDL_EMFS_JS_Prefetch(file->path.c_str(), file->etag.c_str(), Low(file->size), High(file->size), Low(offset), High(offset), Low(length), High(length));
}

void *SDL_EMFS_WebLoad(SDL_EMFS_Context *, const SDL_EMFS_AssetEntry &entry, size_t *size)
{
    void *buffer = nullptr;
    size_t output_size = 0;
    if (!SDL_EMFS_JS_Load(entry.path.c_str(), entry.etag.c_str(), Low(entry.info.size), High(entry.info.size), &buffer, &output_size)) {
        return nullptr;
    }
    if (size) {
        *size = output_size;
    }
    return buffer;
}

void SDL_EMFS_WebReadAsync(SDL_EMFS_AsyncOperation *operation, SDL_EMFS_AsyncFile *file, Uint64 offset, void *destination, Uint64 length)
{
    SDL_EMFS_JS_ReadAsync(operation, file->path.c_str(), file->etag.c_str(), Low(file->size), High(file->size), Low(offset), High(offset), destination, Low(length), High(length));
}

void SDL_EMFS_WebLoadAsync(SDL_EMFS_AsyncOperation *operation, SDL_EMFS_Context *, const SDL_EMFS_AssetEntry &entry)
{
    SDL_EMFS_JS_LoadAsync(operation, entry.path.c_str(), entry.etag.c_str(), Low(entry.info.size), High(entry.info.size));
}

bool SDL_EMFS_SyncUserStorage(void)
{
    return SDL_EMFS_JS_Sync() != 0;
}

void SDL_EMFS_WebSyncAsync(SDL_EMFS_AsyncOperation *operation)
{
    SDL_EMFS_JS_SyncAsync(operation);
}
