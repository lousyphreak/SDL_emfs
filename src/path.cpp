// SPDX-License-Identifier: GPL-3.0-or-later

#include "SDL_emfs_internal.h"

bool SDL_EMFS_NormalizePath(const char *path, std::string &normalized)
{
    if (!path) {
        return SDL_InvalidParamError("path");
    }

    std::vector<std::string> components;
    const char *cursor = path;

    while (*cursor) {
        while (*cursor == '/' || *cursor == '\\') {
            ++cursor;
        }
        if (!*cursor) {
            break;
        }

        const char *start = cursor;
        while (*cursor && *cursor != '/' && *cursor != '\\') {
            ++cursor;
        }

        std::string component(start, cursor);
        if (component == ".") {
            continue;
        }
        if (component == "..") {
            if (components.empty()) {
                return SDL_SetError("Path escapes its SDL_emfs root");
            }
            components.pop_back();
            continue;
        }

        for (char &ch : component) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch + ('a' - 'A'));
            }
        }
        components.emplace_back(std::move(component));
    }

    normalized.clear();
    for (const std::string &component : components) {
        if (!normalized.empty()) {
            normalized += '/';
        }
        normalized += component;
    }
    return true;
}

std::string SDL_EMFS_JoinPath(const std::string &root, const std::string &path)
{
    if (path.empty()) {
        return root;
    }
    if (!root.empty() && (root.back() == '/' || root.back() == '\\')) {
        return root + path;
    }
    return root + '/' + path;
}

bool SDL_EMFS_IsAssetMode(const char *mode)
{
    if (!mode || mode[0] != 'r') {
        return false;
    }
    for (const char *cursor = mode + 1; *cursor; ++cursor) {
        if (*cursor != 'b') {
            return false;
        }
    }
    return true;
}

bool SDL_EMFS_IsReadOnlyMode(const char *mode)
{
    return mode && mode[0] == 'r' && !SDL_strchr(mode, '+');
}

bool SDL_EMFS_IsWritableMode(const char *mode)
{
    return mode && (mode[0] == 'w' || mode[0] == 'a' || SDL_strchr(mode, '+'));
}

bool SDL_EMFS_IsPreservingMode(const char *mode)
{
    return mode && ((mode[0] == 'r' && SDL_strchr(mode, '+')) || mode[0] == 'a');
}
