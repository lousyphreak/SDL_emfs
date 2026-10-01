// SPDX-License-Identifier: GPL-3.0-or-later

#include "SDL_emfs_internal.h"
#include <SDL3/SDL_storage.h>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif

namespace {

bool IsValidRoot(SDL_EMFS_Root root)
{
    if (root != SDL_EMFS_ROOT_ASSET && root != SDL_EMFS_ROOT_USER) {
        return SDL_SetError("Invalid SDL_emfs root");
    }
    return true;
}

struct EnumerateData
{
    std::unordered_set<std::string> names;
};

SDL_EnumerationResult SDLCALL CollectUserName(void *userdata, const char *, const char *fname)
{
    EnumerateData *data = static_cast<EnumerateData *>(userdata);
    std::string normalized;
    if (!SDL_EMFS_NormalizePath(fname, normalized)) {
        return SDL_ENUM_FAILURE;
    }
    data->names.emplace(std::move(normalized));
    return SDL_ENUM_CONTINUE;
}

bool EnumerateResults(const std::unordered_set<std::string> &names, const std::string &path, SDL_EnumerateDirectoryCallback callback, void *userdata)
{
    const std::string dirname = path.empty() ? "/" : path + '/';
    for (const std::string &name : names) {
        const SDL_EnumerationResult result = callback(userdata, dirname.c_str(), name.c_str());
        if (result == SDL_ENUM_SUCCESS) {
            return true;
        }
        if (result == SDL_ENUM_FAILURE) {
            return false;
        }
    }
    return true;
}

} // namespace

Uint64 SDL_EMFS_GetUserStorageSpaceRemaining(SDL_EMFS_Context *context)
{
    SDL_Storage *storage = SDL_OpenFileStorage(context->user_root.c_str());
    const Uint64 available = SDL_GetStorageSpaceRemaining(storage);
    SDL_CloseStorage(storage);
    return available;
}

bool SDL_EMFS_SetUserPathTime(SDL_EMFS_Context *context, const char *path, SDL_Time time)
{
    std::string normalized;
    if (!SDL_EMFS_NormalizePath(path, normalized)) {
        return false;
    }
    const std::string physical = SDL_EMFS_ResolveUser(context, normalized);
#ifdef _WIN32
    const __utimbuf64 times = { time / 1000000000, time / 1000000000 };
    char *wide = SDL_iconv_string("UTF-16LE", "UTF-8", physical.c_str(), physical.size() + 1);
    const int result = _wutime64(reinterpret_cast<const wchar_t *>(wide), &times);
    SDL_free(wide);
#else
    const utimbuf times = { static_cast<time_t>(time / 1000000000), static_cast<time_t>(time / 1000000000) };
    const int result = utime(physical.c_str(), &times);
#endif
    return result == 0 ? SDL_EMFS_SyncUserStorage() : SDL_SetError("Could not set user file time: %s", std::strerror(errno));
}

bool SDL_EMFS_GetPathInfo(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, SDL_PathInfo *info)
{
    SDL_EMFS_ASSERT_READY(context);
    if (!context || !IsValidRoot(root)) {
        return false;
    }
    std::string normalized;
    if (!SDL_EMFS_NormalizePath(path, normalized)) {
        return false;
    }

    if (root == SDL_EMFS_ROOT_ASSET) {
        const auto file = context->assets.find(normalized);
        if (file != context->assets.end()) {
            if (info) {
                *info = file->second.info;
            }
            return true;
        }
        const auto directory = context->asset_directories.find(normalized);
        if (directory != context->asset_directories.end()) {
            if (info) {
                *info = directory->second.info;
            }
            return true;
        }
        return SDL_SetError("Asset path not found: %s", normalized.c_str());
    }

    return SDL_GetPathInfo(SDL_EMFS_ResolveUser(context, normalized).c_str(), info);
}

bool SDL_EMFS_EnumerateDirectory(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, SDL_EnumerateDirectoryCallback callback, void *userdata)
{
    SDL_EMFS_ASSERT_READY(context);
    if (!context || !IsValidRoot(root)) {
        return false;
    }
    if (!callback) {
        return SDL_InvalidParamError("callback");
    }
    std::string normalized;
    if (!SDL_EMFS_NormalizePath(path, normalized)) {
        return false;
    }

    if (root == SDL_EMFS_ROOT_ASSET) {
        const auto directory = context->asset_directories.find(normalized);
        if (directory == context->asset_directories.end()) {
            return SDL_SetError("Asset directory not found: %s", normalized.c_str());
        }
        return EnumerateResults(directory->second.children, normalized, callback, userdata);
    }

    EnumerateData data;
    if (!SDL_EnumerateDirectory(SDL_EMFS_ResolveUser(context, normalized).c_str(), CollectUserName, &data)) {
        return false;
    }
    return EnumerateResults(data.names, normalized, callback, userdata);
}

bool SDL_EMFS_SaveUserFile(SDL_EMFS_Context *context, const char *path, const void *data, size_t size)
{
    SDL_EMFS_ASSERT_READY(context);
    if (!context) {
        return SDL_InvalidParamError("context");
    }
    std::string normalized;
    if (!SDL_EMFS_NormalizePath(path, normalized) || !SDL_EMFS_CreateParentDirectory(context, normalized)) {
        return false;
    }
    if (!SDL_SaveFile(SDL_EMFS_ResolveUser(context, normalized).c_str(), data, size)) {
        return false;
    }
    return SDL_EMFS_SyncUserStorage();
}

bool SDL_EMFS_CreateUserDirectory(SDL_EMFS_Context *context, const char *path)
{
    SDL_EMFS_ASSERT_READY(context);
    if (!context) {
        return SDL_InvalidParamError("context");
    }
    std::string normalized;
    if (!SDL_EMFS_NormalizePath(path, normalized) || !SDL_CreateDirectory(SDL_EMFS_ResolveUser(context, normalized).c_str())) {
        return false;
    }
    return SDL_EMFS_SyncUserStorage();
}

bool SDL_EMFS_RemoveUserPath(SDL_EMFS_Context *context, const char *path)
{
    SDL_EMFS_ASSERT_READY(context);
    if (!context) {
        return SDL_InvalidParamError("context");
    }
    std::string normalized;
    if (!SDL_EMFS_NormalizePath(path, normalized) || !SDL_RemovePath(SDL_EMFS_ResolveUser(context, normalized).c_str())) {
        return false;
    }
    return SDL_EMFS_SyncUserStorage();
}

bool SDL_EMFS_RenameUserPath(SDL_EMFS_Context *context, const char *old_path, const char *new_path)
{
    SDL_EMFS_ASSERT_READY(context);
    if (!context) {
        return SDL_InvalidParamError("context");
    }
    std::string old_normalized;
    std::string new_normalized;
    if (!SDL_EMFS_NormalizePath(old_path, old_normalized) || !SDL_EMFS_NormalizePath(new_path, new_normalized)) {
        return false;
    }
    if (!SDL_EMFS_CreateParentDirectory(context, new_normalized)) {
        return false;
    }
    if (!SDL_EMFS_RenameUserPathWorking(context, old_normalized, new_normalized)) {
        return false;
    }
    return SDL_EMFS_SyncUserStorage();
}

#ifndef __EMSCRIPTEN__
bool SDL_EMFS_SyncUserStorage(void)
{
    return true;
}
#endif
