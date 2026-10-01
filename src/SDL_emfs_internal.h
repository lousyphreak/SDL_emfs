// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SDL_emfs_internal_h_
#define SDL_emfs_internal_h_

#include <SDL3/SDL.h>

#include <SDL_emfs/SDL_emfs_async.h>
#include <SDL_emfs/SDL_emfs_sync.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SDL_EMFS_AssetEntry
{
    std::string path;
    std::string etag;
    SDL_PathInfo info{};
};

struct SDL_EMFS_AssetDirectory
{
    SDL_PathInfo info{};
    std::unordered_set<std::string> children;
};

enum class SDL_EMFS_OperationType
{
    Ready,
    Open,
    Transfer,
    Close,
    Load,
    Completion,
    Save
};

struct SDL_EMFS_AsyncOperation
{
    SDL_EMFS_OperationType type = SDL_EMFS_OperationType::Completion;
    SDL_EMFS_Context *context = nullptr;
    SDL_EMFS_AsyncFile *file = nullptr;
    void *userdata = nullptr;
    SDL_EMFS_ReadyCallback ready_callback = nullptr;
    SDL_EMFS_OpenCallback open_callback = nullptr;
    SDL_EMFS_TransferCallback transfer_callback = nullptr;
    SDL_EMFS_LoadCallback load_callback = nullptr;
    SDL_EMFS_CompletionCallback completion_callback = nullptr;
    SDL_AsyncIOResult result = SDL_ASYNCIO_COMPLETE;
    Uint64 bytes_transferred = 0;
    void *buffer = nullptr;
    size_t size = 0;
    bool success = true;
    bool write = false;
    int stage = 0;
    std::string error;
    std::string path;
    std::string physical_path;
    std::string mode;
#ifndef __EMSCRIPTEN__
    SDL_AsyncIO *save_stream = nullptr;
#endif
};

struct SDL_EMFS_Context
{
    std::string asset_root;
    std::string organization;
    std::string application;
    std::string user_root;
    size_t chunk_size = 0;
    Uint32 prefetch_chunks = 0;
    Uint32 memory_cache_chunks = 0;

    std::unordered_map<std::string, SDL_EMFS_AssetEntry> assets;
    std::unordered_map<std::string, SDL_EMFS_AssetDirectory> asset_directories;
    std::unordered_map<std::string, std::string> user_paths;
    std::mutex user_paths_lock;

    bool initialization_complete = false;
    bool ready = false;
    std::string initialization_error;

#ifndef __EMSCRIPTEN__
    SDL_AsyncIOQueue *async_queue = nullptr;
#else
    SDL_Mutex *completion_lock = nullptr;
    std::vector<SDL_EMFS_AsyncOperation *> completions;
    std::vector<SDL_EMFS_AsyncOperation *> readiness_waiters;
#endif
};

struct SDL_EMFS_File
{
    SDL_EMFS_Context *context = nullptr;
    SDL_IOStream *stream = nullptr;
    std::string path;
    std::string etag;
    Uint64 size = 0;
    Uint64 position = 0;
    SDL_IOStatus status = SDL_IO_STATUS_READY;
    bool asset = false;
    bool writable = false;
};

struct SDL_EMFS_AsyncFile
{
    SDL_EMFS_Context *context = nullptr;
#ifndef __EMSCRIPTEN__
    SDL_AsyncIO *stream = nullptr;
#else
    SDL_IOStream *stream = nullptr;
#endif
    std::string path;
    std::string etag;
    Uint64 size = 0;
    bool asset = false;
    bool writable = false;
    bool busy = false;
};

bool SDL_EMFS_NormalizePath(const char *path, std::string &normalized);
std::string SDL_EMFS_JoinPath(const std::string &root, const std::string &path);
bool SDL_EMFS_BuildIndexes(SDL_EMFS_Context *context);
bool SDL_EMFS_BuildUserIndex(SDL_EMFS_Context *context);
bool SDL_EMFS_ParseManifest(SDL_EMFS_Context *context, const void *data, size_t size);
void SDL_EMFS_BuildAssetDirectories(SDL_EMFS_Context *context);
bool SDL_EMFS_ResolveAsset(SDL_EMFS_Context *context, const std::string &path, std::string &physical_path);
std::string SDL_EMFS_ResolveUser(SDL_EMFS_Context *context, const std::string &path);
bool SDL_EMFS_UserPathExists(SDL_EMFS_Context *context, const std::string &path, std::string &physical_path);
bool SDL_EMFS_RenameUserPathWorking(SDL_EMFS_Context *context, const std::string &old_path, const std::string &new_path);
bool SDL_EMFS_CreateParentDirectory(SDL_EMFS_Context *context, const std::string &path);
bool SDL_EMFS_IsAssetMode(const char *mode);
bool SDL_EMFS_IsReadOnlyMode(const char *mode);
bool SDL_EMFS_IsWritableMode(const char *mode);
bool SDL_EMFS_IsPreservingMode(const char *mode);

SDL_EMFS_File *SDL_EMFS_OpenResolved(SDL_EMFS_Context *context, SDL_EMFS_Root root, const std::string &path, const char *mode);
bool SDL_EMFS_PrepareOpen(SDL_EMFS_Context *context, SDL_EMFS_Root root, const std::string &path, const char *mode, bool &asset, std::string &physical_path);
bool SDL_EMFS_SyncUserStorage(void);

#ifdef __EMSCRIPTEN__
bool SDL_EMFS_WebWaitReady(SDL_EMFS_Context *context);
bool SDL_EMFS_WebRead(SDL_EMFS_File *file, Uint64 offset, void *destination, size_t length, size_t *transferred);
void SDL_EMFS_WebPrefetch(SDL_EMFS_File *file, Uint64 offset, Uint64 length);
void *SDL_EMFS_WebLoad(SDL_EMFS_Context *context, const SDL_EMFS_AssetEntry &entry, size_t *size);
void SDL_EMFS_WebStartInitialization(SDL_EMFS_Context *context);
void SDL_EMFS_WebReadAsync(SDL_EMFS_AsyncOperation *operation, SDL_EMFS_AsyncFile *file, Uint64 offset, void *destination, Uint64 length);
void SDL_EMFS_WebLoadAsync(SDL_EMFS_AsyncOperation *operation, SDL_EMFS_Context *context, const SDL_EMFS_AssetEntry &entry);
void SDL_EMFS_WebSyncAsync(SDL_EMFS_AsyncOperation *operation);
void SDL_EMFS_QueueCompletion(SDL_EMFS_Context *context, SDL_EMFS_AsyncOperation *operation);
#endif

#ifndef NDEBUG
#define SDL_EMFS_ASSERT_READY(context) SDL_assert((context) && (context)->ready)
#else
#define SDL_EMFS_ASSERT_READY(context) ((void)0)
#endif

#endif /* SDL_emfs_internal_h_ */
