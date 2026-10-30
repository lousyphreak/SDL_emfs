// SPDX-License-Identifier: GPL-3.0-or-later

#include "SDL_emfs_internal.h"

#include <cstdlib>
#include <new>

namespace {

[[noreturn]] void FatalSubmission(const char *message)
{
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "SDL_emfs: %s: %s", message, SDL_GetError());
    std::abort();
}

SDL_EMFS_AsyncOperation *NewOperation(SDL_EMFS_Context *context, SDL_EMFS_OperationType type)
{
    SDL_EMFS_AsyncOperation *operation = new (std::nothrow) SDL_EMFS_AsyncOperation;
    if (!operation) {
        SDL_OutOfMemory();
        FatalSubmission("operation allocation failed");
    }
    operation->context = context;
    operation->type = type;
    return operation;
}

bool IsAsyncMode(const char *mode)
{
    return mode && (SDL_strcmp(mode, "r") == 0 || SDL_strcmp(mode, "w") == 0 || SDL_strcmp(mode, "r+") == 0 || SDL_strcmp(mode, "w+") == 0);
}

void SetOperationError(SDL_EMFS_AsyncOperation *operation)
{
    const char *error = SDL_GetError();
    operation->error = (error && *error) ? error : "SDL_emfs asynchronous operation failed";
}

void DeliverOperation(SDL_EMFS_AsyncOperation *operation)
{
    if (!operation->error.empty()) {
        SDL_SetError("%s", operation->error.c_str());
    }

    switch (operation->type) {
    case SDL_EMFS_OperationType::Ready:
        operation->ready_callback(operation->userdata, operation->success);
        break;
    case SDL_EMFS_OperationType::Open: {
        SDL_EMFS_AsyncFile *file = nullptr;
#ifdef __EMSCRIPTEN__
        if (operation->success) {
            operation->success = SDL_EMFS_CreateParentDirectory(operation->context, operation->path)
                && SDL_SaveFile(operation->physical_path.c_str(), operation->buffer, operation->size);
            if (!operation->success) {
                SetOperationError(operation);
            }
        }
        SDL_free(operation->buffer);
        operation->buffer = nullptr;
        if (operation->success) {
            file = new (std::nothrow) SDL_EMFS_AsyncFile;
            if (!file) {
                SDL_OutOfMemory();
                FatalSubmission("async file allocation failed");
            }
            file->context = operation->context;
            file->path = operation->path;
            file->writable = true;
            file->stream = SDL_IOFromFile(operation->physical_path.c_str(), operation->mode.c_str());
            if (!file->stream) {
                delete file;
                file = nullptr;
                operation->success = false;
                SetOperationError(operation);
            } else {
                const Sint64 size = SDL_GetIOSize(file->stream);
                file->size = size >= 0 ? (Uint64)size : 0;
            }
        }
#endif
        if (!operation->success && !operation->error.empty()) {
            SDL_SetError("%s", operation->error.c_str());
        }
        operation->open_callback(operation->userdata, file);
        break;
    }
    case SDL_EMFS_OperationType::Transfer:
        operation->file->busy = false;
        operation->transfer_callback(operation->userdata, operation->result, operation->bytes_transferred);
        break;
    case SDL_EMFS_OperationType::Close: {
        SDL_EMFS_CompletionCallback callback = operation->completion_callback;
        void *userdata = operation->userdata;
        delete operation->file;
        operation->file = nullptr;
        callback(userdata, operation->success);
        break;
    }
    case SDL_EMFS_OperationType::Load:
#ifdef SDL_PLATFORM_WIIU
        // Native FS callbacks run on AppIO. Synchronous close must run here on
        // the Pump caller, where its completion can still be dispatched.
        if (operation->file) {
            SDL_EMFS_WiiUCloseAsyncAsset(operation->file->wiiu_asset);
            delete operation->file;
            operation->file = nullptr;
        }
#endif
        operation->load_callback(operation->userdata, operation->result, operation->buffer, operation->size);
        break;
    case SDL_EMFS_OperationType::Completion:
    case SDL_EMFS_OperationType::Save:
        operation->completion_callback(operation->userdata, operation->success);
        break;
    }
    delete operation;
}

#ifndef __EMSCRIPTEN__

void SubmitSaveClose(SDL_EMFS_AsyncOperation *operation)
{
    operation->stage = 1;
    if (!SDL_CloseAsyncIO(operation->save_stream, true, operation->context->async_queue, operation)) {
        FatalSubmission("could not submit async save close");
    }
}

void ProcessNativeOutcome(const SDL_AsyncIOOutcome &outcome)
{
    SDL_EMFS_AsyncOperation *operation = static_cast<SDL_EMFS_AsyncOperation *>(outcome.userdata);
    if (operation->type == SDL_EMFS_OperationType::Save) {
        if (operation->stage == 0) {
            if (outcome.result != SDL_ASYNCIO_COMPLETE) {
                operation->success = false;
                SetOperationError(operation);
            }
            SubmitSaveClose(operation);
            return;
        }
        if (outcome.result != SDL_ASYNCIO_COMPLETE) {
            operation->success = false;
            if (operation->error.empty()) {
                SetOperationError(operation);
            }
        }
        DeliverOperation(operation);
        return;
    }

    operation->result = outcome.result;
    operation->bytes_transferred = outcome.bytes_transferred;
    operation->success = outcome.result == SDL_ASYNCIO_COMPLETE;
    if (!operation->success) {
        SetOperationError(operation);
    }
    if (operation->type == SDL_EMFS_OperationType::Load) {
        operation->buffer = operation->success ? outcome.buffer : nullptr;
        operation->size = operation->success ? (size_t)outcome.bytes_transferred : 0;
        if (!operation->success) {
            SDL_free(outcome.buffer);
        }
    }
    DeliverOperation(operation);
}

#endif

} // namespace

void SDL_EMFS_ReadyAsync(SDL_EMFS_Context *context, SDL_EMFS_ReadyCallback callback, void *userdata)
{
    SDL_assert(callback != nullptr);
    if (!context) {
        SDL_InvalidParamError("context");
        callback(userdata, false);
        return;
    }
#ifdef __EMSCRIPTEN__
    SDL_LockMutex(context->completion_lock);
    if (!context->initialization_complete) {
        SDL_EMFS_AsyncOperation *operation = NewOperation(context, SDL_EMFS_OperationType::Ready);
        operation->ready_callback = callback;
        operation->userdata = userdata;
        context->readiness_waiters.emplace_back(operation);
        SDL_UnlockMutex(context->completion_lock);
        return;
    }
    const bool ready = context->ready;
    const std::string initialization_error = context->initialization_error;
    SDL_UnlockMutex(context->completion_lock);
    if (!ready && !initialization_error.empty()) {
        SDL_SetError("%s", initialization_error.c_str());
    }
    callback(userdata, ready);
    return;
#endif
    if (!context->ready && !context->initialization_error.empty()) {
        SDL_SetError("%s", context->initialization_error.c_str());
    }
    callback(userdata, context->ready);
}

void SDL_EMFS_OpenAsync(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, const char *mode, SDL_EMFS_OpenCallback callback, void *userdata)
{
    SDL_EMFS_ASSERT_READY(context);
    SDL_assert(callback != nullptr);
    if (!context || !IsAsyncMode(mode)) {
        if (context) {
            SDL_SetError("Invalid SDL_emfs async file mode");
        } else {
            SDL_InvalidParamError("context");
        }
        callback(userdata, nullptr);
        return;
    }
    std::string normalized;
#ifdef __EMSCRIPTEN__
    if (SDL_EMFS_NormalizePath(path, normalized) && root == SDL_EMFS_ROOT_USER && SDL_strcmp(mode, "r+") == 0) {
        std::string physical_path;
        if (!SDL_EMFS_UserPathExists(context, normalized, physical_path)) {
            const auto entry = context->assets.find(normalized);
            if (entry != context->assets.end()) {
                SDL_EMFS_AsyncOperation *operation = NewOperation(context, SDL_EMFS_OperationType::Open);
                operation->open_callback = callback;
                operation->userdata = userdata;
                operation->path = normalized;
                operation->physical_path = std::move(physical_path);
                operation->mode = mode;
                SDL_EMFS_WebLoadAsync(operation, context, entry->second);
                return;
            }
        }
    }
#endif
    bool asset = false;
    std::string physical_path;
    if (!SDL_EMFS_NormalizePath(path, normalized) || !SDL_EMFS_PrepareOpen(context, root, normalized, mode, asset, physical_path)) {
        callback(userdata, nullptr);
        return;
    }

    SDL_EMFS_AsyncFile *file = new (std::nothrow) SDL_EMFS_AsyncFile;
    if (!file) {
        SDL_OutOfMemory();
        FatalSubmission("async file allocation failed");
    }
    file->context = context;
    file->path = normalized;
    file->asset = asset;
    file->writable = SDL_EMFS_IsWritableMode(mode);
    if (asset) {
        const SDL_EMFS_AssetEntry &entry = context->assets.find(normalized)->second;
        file->size = entry.info.size;
        file->etag = entry.etag;
    }
#ifdef __EMSCRIPTEN__
    if (!asset) {
        file->stream = SDL_IOFromFile(physical_path.c_str(), mode);
        if (!file->stream) {
            delete file;
            callback(userdata, nullptr);
            return;
        }
        const Sint64 size = SDL_GetIOSize(file->stream);
        file->size = size >= 0 ? (Uint64)size : 0;
    }
#else
#ifdef SDL_PLATFORM_WIIU
    if (asset) {
        file->wiiu_asset = SDL_EMFS_WiiUOpenAsyncAsset(context->assets.find(normalized)->second);
        if (file->wiiu_asset) {
            callback(userdata, file);
            return;
        }
    }
#endif
    file->stream = SDL_AsyncIOFromFile(physical_path.c_str(), mode);
    if (!file->stream) {
        delete file;
        callback(userdata, nullptr);
        return;
    }
    const Sint64 size = SDL_GetAsyncIOSize(file->stream);
    file->size = size >= 0 ? (Uint64)size : 0;
#endif
    callback(userdata, file);
}

Sint64 SDL_EMFS_GetAsyncFileSize(SDL_EMFS_AsyncFile *file)
{
    if (!file) {
        SDL_InvalidParamError("file");
        return -1;
    }
    SDL_assert(!file->busy);
#ifdef SDL_PLATFORM_WIIU
    if (file->wiiu_asset) return file->size;
#endif
#ifdef __EMSCRIPTEN__
    return file->asset ? (Sint64)file->size : SDL_GetIOSize(file->stream);
#else
    return SDL_GetAsyncIOSize(file->stream);
#endif
}

void SDL_EMFS_ReadAtAsync(SDL_EMFS_AsyncFile *file, Uint64 offset, void *destination, Uint64 length, SDL_EMFS_TransferCallback callback, void *userdata)
{
    SDL_assert(file && !file->busy && callback);
    file->busy = true;
    SDL_EMFS_AsyncOperation *operation = NewOperation(file->context, SDL_EMFS_OperationType::Transfer);
    operation->file = file;
    operation->transfer_callback = callback;
    operation->userdata = userdata;
#ifdef __EMSCRIPTEN__
    if (file->asset) {
        SDL_EMFS_WebReadAsync(operation, file, offset, destination, length);
    } else {
        if (offset > INT64_MAX || SDL_SeekIO(file->stream, (Sint64)offset, SDL_IO_SEEK_SET) < 0) {
            operation->result = SDL_ASYNCIO_FAILURE;
            SetOperationError(operation);
        } else {
            operation->bytes_transferred = SDL_ReadIO(file->stream, destination, (size_t)length);
            operation->result = SDL_GetIOStatus(file->stream) == SDL_IO_STATUS_ERROR ? SDL_ASYNCIO_FAILURE : SDL_ASYNCIO_COMPLETE;
            if (operation->result == SDL_ASYNCIO_FAILURE) {
                SetOperationError(operation);
            }
        }
        SDL_EMFS_QueueCompletion(file->context, operation);
    }
#else
#ifdef SDL_PLATFORM_WIIU
    if (file->wiiu_asset) {
        operation->buffer = destination;
        operation->size = SDL_min(length, offset < file->size ? file->size - offset : 0);
        SDL_EMFS_WiiUReadAsync(operation, offset);
        return;
    }
#endif
    if (!SDL_ReadAsyncIO(file->stream, destination, offset, length, file->context->async_queue, operation)) {
        FatalSubmission("could not submit async read");
    }
#endif
}

void SDL_EMFS_WriteAtAsync(SDL_EMFS_AsyncFile *file, Uint64 offset, const void *source, Uint64 length, SDL_EMFS_TransferCallback callback, void *userdata)
{
    SDL_assert(file && !file->busy && callback);
    file->busy = true;
    SDL_EMFS_AsyncOperation *operation = NewOperation(file->context, SDL_EMFS_OperationType::Transfer);
    operation->file = file;
    operation->transfer_callback = callback;
    operation->userdata = userdata;
    operation->write = true;
#ifdef __EMSCRIPTEN__
    if (file->asset) {
        operation->result = SDL_ASYNCIO_FAILURE;
        operation->error = "Asset files are read-only";
    } else if (offset > INT64_MAX || SDL_SeekIO(file->stream, (Sint64)offset, SDL_IO_SEEK_SET) < 0) {
        operation->result = SDL_ASYNCIO_FAILURE;
        SetOperationError(operation);
    } else {
        operation->bytes_transferred = SDL_WriteIO(file->stream, source, (size_t)length);
        operation->result = operation->bytes_transferred == length ? SDL_ASYNCIO_COMPLETE : SDL_ASYNCIO_FAILURE;
        if (operation->result == SDL_ASYNCIO_FAILURE) {
            SetOperationError(operation);
        }
    }
    SDL_EMFS_QueueCompletion(file->context, operation);
#else
#ifdef SDL_PLATFORM_WIIU
    if (file->wiiu_asset) {
        operation->result = SDL_ASYNCIO_FAILURE;
        operation->error = "Asset files are read-only";
        SDL_EMFS_QueueCompletion(file->context, operation);
        return;
    }
#endif
    if (!SDL_WriteAsyncIO(file->stream, const_cast<void *>(source), offset, length, file->context->async_queue, operation)) {
        FatalSubmission("could not submit async write");
    }
#endif
}

void SDL_EMFS_CloseAsync(SDL_EMFS_AsyncFile *file, SDL_EMFS_CompletionCallback callback, void *userdata)
{
    SDL_assert(file && !file->busy && callback);
    file->busy = true;
    SDL_EMFS_AsyncOperation *operation = NewOperation(file->context, SDL_EMFS_OperationType::Close);
    operation->file = file;
    operation->completion_callback = callback;
    operation->userdata = userdata;
#ifdef __EMSCRIPTEN__
    if (file->stream && !SDL_CloseIO(file->stream)) {
        operation->success = false;
        SetOperationError(operation);
    }
    file->stream = nullptr;
    if (file->writable) {
        SDL_EMFS_WebSyncAsync(operation);
    } else {
        SDL_EMFS_QueueCompletion(file->context, operation);
    }
#else
#ifdef SDL_PLATFORM_WIIU
    if (file->wiiu_asset) {
        operation->success = SDL_EMFS_WiiUCloseAsyncAsset(file->wiiu_asset);
        file->wiiu_asset = nullptr;
        if (!operation->success) SetOperationError(operation);
        SDL_EMFS_QueueCompletion(file->context, operation);
        return;
    }
#endif
    if (!SDL_CloseAsyncIO(file->stream, file->writable, file->context->async_queue, operation)) {
        FatalSubmission("could not submit async close");
    }
#endif
}

void SDL_EMFS_LoadFileAsync(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, SDL_EMFS_LoadCallback callback, void *userdata)
{
    SDL_EMFS_ASSERT_READY(context);
    SDL_assert(callback != nullptr);
    std::string normalized;
    if (!context || !SDL_EMFS_NormalizePath(path, normalized)) {
        callback(userdata, SDL_ASYNCIO_FAILURE, nullptr, 0);
        return;
    }

    std::string physical_path;
    bool asset = root == SDL_EMFS_ROOT_ASSET;
    if (root == SDL_EMFS_ROOT_USER) {
        asset = !SDL_EMFS_UserPathExists(context, normalized, physical_path);
    } else if (root != SDL_EMFS_ROOT_ASSET) {
        SDL_SetError("Invalid SDL_emfs root");
        callback(userdata, SDL_ASYNCIO_FAILURE, nullptr, 0);
        return;
    }
    const auto entry = context->assets.find(normalized);
    if (asset && entry == context->assets.end()) {
        SDL_SetError("Asset not found: %s", normalized.c_str());
        callback(userdata, SDL_ASYNCIO_FAILURE, nullptr, 0);
        return;
    }
    if (asset) {
#ifndef __EMSCRIPTEN__
        physical_path = entry->second.path;
#endif
    }

    SDL_EMFS_AsyncOperation *operation = NewOperation(context, SDL_EMFS_OperationType::Load);
    operation->load_callback = callback;
    operation->userdata = userdata;
#ifdef __EMSCRIPTEN__
    if (asset) {
        SDL_EMFS_WebLoadAsync(operation, context, entry->second);
    } else {
        operation->buffer = SDL_LoadFile(physical_path.c_str(), &operation->size);
        operation->result = operation->buffer ? SDL_ASYNCIO_COMPLETE : SDL_ASYNCIO_FAILURE;
        if (!operation->buffer) {
            SetOperationError(operation);
        }
        SDL_EMFS_QueueCompletion(context, operation);
    }
#else
#ifdef SDL_PLATFORM_WIIU
    if (asset && SDL_EMFS_WiiULoadAsync(operation, entry->second)) return;
#endif
    if (!SDL_LoadFileAsync(physical_path.c_str(), context->async_queue, operation)) {
        operation->result = SDL_ASYNCIO_FAILURE;
        SetOperationError(operation);
        DeliverOperation(operation);
    }
#endif
}

void SDL_EMFS_SaveUserFileAsync(SDL_EMFS_Context *context, const char *path, const void *data, size_t size, SDL_EMFS_CompletionCallback callback, void *userdata)
{
    SDL_EMFS_ASSERT_READY(context);
    SDL_assert(callback != nullptr);
    std::string normalized;
    if (!context || !SDL_EMFS_NormalizePath(path, normalized) || !SDL_EMFS_CreateParentDirectory(context, normalized)) {
        callback(userdata, false);
        return;
    }
    const std::string physical_path = SDL_EMFS_ResolveUser(context, normalized);
    SDL_EMFS_AsyncOperation *operation = NewOperation(context, SDL_EMFS_OperationType::Save);
    operation->completion_callback = callback;
    operation->userdata = userdata;
#ifdef __EMSCRIPTEN__
    operation->success = SDL_SaveFile(physical_path.c_str(), data, size);
    if (!operation->success) {
        SetOperationError(operation);
        SDL_EMFS_QueueCompletion(context, operation);
    } else {
        SDL_EMFS_WebSyncAsync(operation);
    }
#else
    operation->save_stream = SDL_AsyncIOFromFile(physical_path.c_str(), "w");
    if (!operation->save_stream) {
        SetOperationError(operation);
        operation->success = false;
        DeliverOperation(operation);
        return;
    }
    if (size) {
        if (!SDL_WriteAsyncIO(operation->save_stream, const_cast<void *>(data), 0, size, context->async_queue, operation)) {
            FatalSubmission("could not submit async save write");
        }
    } else {
        SubmitSaveClose(operation);
    }
#endif
}

void SDL_EMFS_CreateUserDirectoryAsync(SDL_EMFS_Context *context, const char *path, SDL_EMFS_CompletionCallback callback, void *userdata)
{
    SDL_EMFS_ASSERT_READY(context);
    SDL_assert(callback != nullptr);
#ifndef __EMSCRIPTEN__
    callback(userdata, SDL_EMFS_CreateUserDirectory(context, path));
#else
    std::string normalized;
    SDL_EMFS_AsyncOperation *operation = NewOperation(context, SDL_EMFS_OperationType::Completion);
    operation->completion_callback = callback;
    operation->userdata = userdata;
    operation->success = SDL_EMFS_NormalizePath(path, normalized) && SDL_CreateDirectory(SDL_EMFS_ResolveUser(context, normalized).c_str());
    if (!operation->success) {
        SetOperationError(operation);
        SDL_EMFS_QueueCompletion(context, operation);
    } else {
        SDL_EMFS_WebSyncAsync(operation);
    }
#endif
}

void SDL_EMFS_RemoveUserPathAsync(SDL_EMFS_Context *context, const char *path, SDL_EMFS_CompletionCallback callback, void *userdata)
{
    SDL_EMFS_ASSERT_READY(context);
    SDL_assert(callback != nullptr);
#ifndef __EMSCRIPTEN__
    callback(userdata, SDL_EMFS_RemoveUserPath(context, path));
#else
    std::string normalized;
    SDL_EMFS_AsyncOperation *operation = NewOperation(context, SDL_EMFS_OperationType::Completion);
    operation->completion_callback = callback;
    operation->userdata = userdata;
    operation->success = SDL_EMFS_NormalizePath(path, normalized) && SDL_RemovePath(SDL_EMFS_ResolveUser(context, normalized).c_str());
    if (!operation->success) {
        SetOperationError(operation);
        SDL_EMFS_QueueCompletion(context, operation);
    } else {
        SDL_EMFS_WebSyncAsync(operation);
    }
#endif
}

void SDL_EMFS_RenameUserPathAsync(SDL_EMFS_Context *context, const char *old_path, const char *new_path, SDL_EMFS_CompletionCallback callback, void *userdata)
{
    SDL_EMFS_ASSERT_READY(context);
    SDL_assert(callback != nullptr);
#ifndef __EMSCRIPTEN__
    callback(userdata, SDL_EMFS_RenameUserPath(context, old_path, new_path));
#else
    std::string old_normalized;
    std::string new_normalized;
    SDL_EMFS_AsyncOperation *operation = NewOperation(context, SDL_EMFS_OperationType::Completion);
    operation->completion_callback = callback;
    operation->userdata = userdata;
    operation->success = SDL_EMFS_NormalizePath(old_path, old_normalized) && SDL_EMFS_NormalizePath(new_path, new_normalized)
        && SDL_EMFS_CreateParentDirectory(context, new_normalized)
        && SDL_EMFS_RenameUserPathWorking(context, old_normalized, new_normalized);
    if (!operation->success) {
        SetOperationError(operation);
        SDL_EMFS_QueueCompletion(context, operation);
    } else {
        SDL_EMFS_WebSyncAsync(operation);
    }
#endif
}

void SDL_EMFS_Pump(SDL_EMFS_Context *context)
{
    if (!context) {
        return;
    }
#if defined(__EMSCRIPTEN__) || defined(SDL_PLATFORM_WIIU)
    std::vector<SDL_EMFS_AsyncOperation *> completions;
    SDL_LockMutex(context->completion_lock);
    completions.swap(context->completions);
    SDL_UnlockMutex(context->completion_lock);
    for (size_t index = 0; index < completions.size(); ++index) {
        SDL_EMFS_AsyncOperation *operation = completions[index];
        DeliverOperation(operation);
        SDL_LockMutex(context->completion_lock);
        if (!context->completions.empty()) {
            completions.insert(completions.end(), context->completions.begin(), context->completions.end());
            context->completions.clear();
        }
        SDL_UnlockMutex(context->completion_lock);
    }
#endif
#ifndef __EMSCRIPTEN__
    SDL_AsyncIOOutcome outcome;
    while (SDL_GetAsyncIOResult(context->async_queue, &outcome)) {
        ProcessNativeOutcome(outcome);
    }
#endif
}
