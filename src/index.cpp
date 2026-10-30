// SPDX-License-Identifier: GPL-3.0-or-later

#include "SDL_emfs_internal.h"

#include <cstring>

#ifdef SDL_PLATFORM_WIIU
#include <coreinit/filesystem.h>
#endif

namespace {

struct ScanData
{
    SDL_EMFS_Context *context;
    std::string logical_directory;
    bool asset;
    bool success;
};

bool ScanDirectory(SDL_EMFS_Context *context, const std::string &physical_directory, const std::string &logical_directory, bool asset);

#ifndef SDL_PLATFORM_WIIU
SDL_EnumerationResult IndexEntry(ScanData *data, const char *dirname, const char *fname, const SDL_PathInfo &info)
{
    const std::string physical_path = std::string(dirname) + fname;
    std::string logical_path;
    const std::string unnormalized = data->logical_directory.empty() ? fname : data->logical_directory + '/' + fname;
    if (!SDL_EMFS_NormalizePath(unnormalized.c_str(), logical_path)) {
        data->success = false;
        return SDL_ENUM_FAILURE;
    }

    if (data->asset) {
        if (info.type == SDL_PATHTYPE_FILE) {
            SDL_EMFS_AssetEntry entry;
            entry.path = physical_path;
            entry.info = info;
            data->context->assets.emplace(logical_path, std::move(entry));
        } else if (info.type == SDL_PATHTYPE_DIRECTORY && !ScanDirectory(data->context, physical_path, logical_path, true)) {
            data->success = false;
            return SDL_ENUM_FAILURE;
        }
    } else {
        data->context->user_paths.emplace(logical_path, physical_path);
        if (info.type == SDL_PATHTYPE_DIRECTORY && !ScanDirectory(data->context, physical_path, logical_path, false)) {
            data->success = false;
            return SDL_ENUM_FAILURE;
        }
    }

    return SDL_ENUM_CONTINUE;
}

SDL_EnumerationResult SDLCALL ScanCallback(void *userdata, const char *dirname, const char *fname)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo((std::string(dirname) + fname).c_str(), &info)) {
        static_cast<ScanData *>(userdata)->success = false;
        return SDL_ENUM_FAILURE;
    }
    return IndexEntry(static_cast<ScanData *>(userdata), dirname, fname, info);
}
#else
// Reuse one native client through recursive scans. Each level has its own
// command block and directory handle, so recursion preserves directory position.
bool ScanWiiUDirectory(ScanData *data, const std::string &path, FSClient *client)
{
    FSCmdBlock block;
    FSInitCmdBlock(&block);
    FSDirectoryHandle handle;
    FSStatus status = FSOpenDir(client, &block, path.c_str(), &handle, FS_ERROR_FLAG_ALL);
    if (status != FS_STATUS_OK) {
        return SDL_SetError("FSOpenDir(%s) failed: %d", path.c_str(), status);
    }
    const std::string dirname = path + (path.back() == '/' ? "" : "/");
    bool success = true;
    for (;;) {
        FSDirectoryEntry entry{};
        status = FSReadDir(client, &block, handle, &entry, FS_ERROR_FLAG_ALL);
        if (status == FS_STATUS_END) {
            break;
        }
        if (status != FS_STATUS_OK) {
            success = SDL_SetError("FSReadDir(%s) failed: %d", path.c_str(), status);
            break;
        }
        if (SDL_strcmp(entry.name, ".") == 0 || SDL_strcmp(entry.name, "..") == 0) {
            continue;
        }
        const FSStat &stat = entry.info;
        SDL_PathInfo info{};
        info.type = (stat.flags & FS_STAT_LINK) ? SDL_PATHTYPE_OTHER :
                    (stat.flags & FS_STAT_DIRECTORY) ? SDL_PATHTYPE_DIRECTORY : SDL_PATHTYPE_FILE;
        info.size = info.type == SDL_PATHTYPE_DIRECTORY ? 0 : stat.size;
        info.create_time = (static_cast<SDL_Time>(stat.created) + 315532800000000LL) * 1000;
        info.modify_time = (static_cast<SDL_Time>(stat.modified) + 315532800000000LL) * 1000;
        info.access_time = info.modify_time;
        // Directory metadata belongs to this entry already. Do not resolve the
        // name again with SDL_GetPathInfo/FSGetStat.
        std::string logical;
        const std::string unnormalized = data->logical_directory.empty() ? entry.name : data->logical_directory + '/' + entry.name;
        if (!SDL_EMFS_NormalizePath(unnormalized.c_str(), logical)) {
            success = false;
            break;
        }
        if (data->asset && info.type == SDL_PATHTYPE_FILE) {
            SDL_EMFS_AssetEntry asset;
            asset.path = dirname + entry.name;
            asset.info = info;
            asset.native_stat = stat;
            data->context->assets.emplace(logical, std::move(asset));
        } else if (!data->asset) {
            data->context->user_paths.emplace(logical, dirname + entry.name);
        }
        if (info.type == SDL_PATHTYPE_DIRECTORY) {
            ScanData child{data->context, logical, data->asset, true};
            if (!ScanWiiUDirectory(&child, dirname + entry.name, client)) {
                success = false;
                break;
            }
        }
    }
    const FSStatus closed = FSCloseDir(client, &block, handle, FS_ERROR_FLAG_ALL);
    return success && (closed == FS_STATUS_OK || SDL_SetError("FSCloseDir failed: %d", closed));
}
#endif

bool ScanDirectory(SDL_EMFS_Context *context, const std::string &physical_directory, const std::string &logical_directory, bool asset)
{
    ScanData data{ context, logical_directory, asset, true };
#ifdef SDL_PLATFORM_WIIU
    FSInit();
    auto *client = static_cast<FSClient *>(SDL_calloc(1, sizeof(FSClient)));
    if (!client) {
        return false;
    }
    const FSStatus status = FSAddClient(client, FS_ERROR_FLAG_ALL);
    if (status != FS_STATUS_OK) {
        SDL_free(client);
        return SDL_SetError("FSAddClient failed: %d", status);
    }
    // Match SDL's native relative-path base without changing the supplied root.
    const std::string path = physical_directory.front() == '/' ? physical_directory : "/vol/content/" + physical_directory;
    const bool success = ScanWiiUDirectory(&data, path, client);
    FSDelClient(client, FS_ERROR_FLAG_ALL);
    SDL_free(client);
    return success;
#else
    return SDL_EnumerateDirectory(physical_directory.c_str(), ScanCallback, &data) && data.success;
#endif
}

Uint32 ReadU32(const Uint8 *data)
{
    return (Uint32)data[0] | ((Uint32)data[1] << 8) | ((Uint32)data[2] << 16) | ((Uint32)data[3] << 24);
}

Uint64 ReadU64(const Uint8 *data)
{
    return (Uint64)ReadU32(data) | ((Uint64)ReadU32(data + 4) << 32);
}

} // namespace

void SDL_EMFS_BuildAssetDirectories(SDL_EMFS_Context *context)
{
    context->asset_directories.clear();
    SDL_EMFS_AssetDirectory &root = context->asset_directories[""];
    root.info.type = SDL_PATHTYPE_DIRECTORY;

    for (const auto &item : context->assets) {
        const std::string &path = item.first;
        size_t start = 0;
        std::string directory;

        for (;;) {
            const size_t slash = path.find('/', start);
            const bool is_file = slash == std::string::npos;
            const std::string name = path.substr(start, is_file ? std::string::npos : slash - start);
            SDL_EMFS_AssetDirectory &parent = context->asset_directories[directory];
            parent.info.type = SDL_PATHTYPE_DIRECTORY;
            parent.children.emplace(name);

            if (is_file) {
                if (item.second.info.modify_time > parent.info.modify_time) {
                    parent.info.modify_time = item.second.info.modify_time;
                }
                break;
            }

            directory = directory.empty() ? name : directory + '/' + name;
            SDL_EMFS_AssetDirectory &child = context->asset_directories[directory];
            child.info.type = SDL_PATHTYPE_DIRECTORY;
            start = slash + 1;
        }
    }
}

bool SDL_EMFS_BuildIndexes(SDL_EMFS_Context *context)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(context->asset_root.c_str(), &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        return SDL_SetError("Asset root is not a directory: %s", context->asset_root.c_str());
    }
    if (!ScanDirectory(context, context->asset_root, "", true)) {
        return false;
    }
    SDL_EMFS_BuildAssetDirectories(context);

    return SDL_EMFS_BuildUserIndex(context);
}

bool SDL_EMFS_BuildUserIndex(SDL_EMFS_Context *context)
{
    context->user_paths.clear();
    return ScanDirectory(context, context->user_root, "", false);
}

bool SDL_EMFS_ParseManifest(SDL_EMFS_Context *context, const void *data, size_t size)
{
    const Uint8 *cursor = static_cast<const Uint8 *>(data);
    if (size < 12 || SDL_memcmp(cursor, "SDLEMFS1", 8) != 0) {
        return SDL_SetError("Invalid SDL_emfs asset manifest");
    }
    const Uint8 *end = cursor + size;
    cursor += 8;
    const Uint32 count = ReadU32(cursor);
    cursor += 4;

    context->assets.clear();
    for (Uint32 i = 0; i < count; ++i) {
        if ((size_t)(end - cursor) < 24) {
            return SDL_SetError("Truncated SDL_emfs asset manifest");
        }
        const Uint64 file_size = ReadU64(cursor);
        cursor += 8;
        const Sint64 modify_ms = (Sint64)ReadU64(cursor);
        cursor += 8;
        const Uint32 path_length = ReadU32(cursor);
        cursor += 4;
        const Uint32 etag_length = ReadU32(cursor);
        cursor += 4;
        if ((Uint64)(end - cursor) < (Uint64)path_length + etag_length || file_size >= (UINT64_C(1) << 53)) {
            return SDL_SetError("Invalid SDL_emfs asset manifest record");
        }

        SDL_EMFS_AssetEntry entry;
        const std::string manifest_path(reinterpret_cast<const char *>(cursor), path_length);
        cursor += path_length;
        entry.etag.assign(reinterpret_cast<const char *>(cursor), etag_length);
        cursor += etag_length;
        entry.info.type = SDL_PATHTYPE_FILE;
        entry.path = manifest_path;
        entry.info.size = file_size;
        entry.info.modify_time = modify_ms * INT64_C(1000000);

        std::string normalized;
        if (!SDL_EMFS_NormalizePath(manifest_path.c_str(), normalized) || normalized != manifest_path || normalized.empty()) {
            return SDL_SetError("Invalid path in SDL_emfs asset manifest");
        }
        context->assets.emplace(normalized, std::move(entry));
    }
    if (cursor != end) {
        return SDL_SetError("Trailing data in SDL_emfs asset manifest");
    }

    SDL_EMFS_BuildAssetDirectories(context);
    return true;
}

bool SDL_EMFS_ResolveAsset(SDL_EMFS_Context *context, const std::string &path, std::string &physical_path)
{
    const auto found = context->assets.find(path);
    if (found == context->assets.end()) {
        return SDL_SetError("Asset not found: %s", path.c_str());
    }
#ifdef __EMSCRIPTEN__
    physical_path = path;
#else
    physical_path = found->second.path;
#endif
    return true;
}

std::string SDL_EMFS_ResolveUser(SDL_EMFS_Context *context, const std::string &path)
{
    // New files must use the actual case of an existing parent, too. Otherwise
    // writing data/save/new.sav beside a migrated DATA/SAVE creates a second tree
    // that enumeration of the original directory cannot see.
    std::lock_guard<std::mutex> lock(context->user_paths_lock);
    std::string ancestor = path;
    for (;;) {
        const auto found = context->user_paths.find(ancestor);
        if (found != context->user_paths.end()) {
            SDL_PathInfo info;
            if (SDL_GetPathInfo(found->second.c_str(), &info)) {
                return found->second + path.substr(ancestor.size());
            }
            SDL_ClearError();
        }
        const size_t slash = ancestor.rfind('/');
        if (slash == std::string::npos) break;
        ancestor.resize(slash);
    }
    return SDL_EMFS_JoinPath(context->user_root, path);
}

bool SDL_EMFS_UserPathExists(SDL_EMFS_Context *context, const std::string &path, std::string &physical_path)
{
    physical_path = SDL_EMFS_ResolveUser(context, path);
    SDL_PathInfo info;
    const bool exists = SDL_GetPathInfo(physical_path.c_str(), &info);
    if (!exists) {
        SDL_ClearError();
    }
    return exists;
}

bool SDL_EMFS_CreateParentDirectory(SDL_EMFS_Context *context, const std::string &path)
{
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return true;
    }
    return SDL_CreateDirectory(SDL_EMFS_ResolveUser(context, path.substr(0, slash)).c_str());
}

bool SDL_EMFS_RenameUserPathWorking(SDL_EMFS_Context *context, const std::string &old_normalized, const std::string &new_normalized)
{
    const std::string old_physical = SDL_EMFS_ResolveUser(context, old_normalized);
    const std::string new_physical = SDL_EMFS_ResolveUser(context, new_normalized);
    if (!SDL_RenamePath(old_physical.c_str(), new_physical.c_str())) {
        return false;
    }
    std::lock_guard<std::mutex> lock(context->user_paths_lock);
    std::vector<std::pair<std::string, std::string>> renamed;
    for (auto it = context->user_paths.begin(); it != context->user_paths.end();) {
        if (it->first == old_normalized || it->first.compare(0, old_normalized.size() + 1, old_normalized + '/') == 0) {
            renamed.emplace_back(new_normalized + it->first.substr(old_normalized.size()),
                new_physical + it->second.substr(old_physical.size()));
            it = context->user_paths.erase(it);
        } else {
            ++it;
        }
    }
    for (auto &entry : renamed) context->user_paths[entry.first] = std::move(entry.second);
    return true;
}
