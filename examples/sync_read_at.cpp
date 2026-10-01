// SPDX-License-Identifier: GPL-3.0-or-later

#include <SDL3/SDL.h>
#include <SDL_emfs/SDL_emfs_sync.h>

int main(int argc, char **argv)
{
    SDL_EMFS_Config config;
    SDL_EMFS_InitConfig(&config);
    config.asset_root = argc > 1 ? argv[1] : ".";
    config.application = "read-at-example";
    SDL_EMFS_Context *context = SDL_EMFS_Create(&config);
    if (!context || !SDL_EMFS_WaitReady(context)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        return 1;
    }
    const char *asset_path = argc > 2 ? argv[2] : "sdl_emfs_sync_read_at.js";
    SDL_EMFS_File *file = SDL_EMFS_Open(context, SDL_EMFS_ROOT_ASSET, asset_path, "rb");
    Uint8 bytes[16];
    SDL_IOStatus status;
    if (!file || SDL_EMFS_ReadAt(file, 0, bytes, sizeof(bytes), &status) == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        return 1;
    }
    SDL_EMFS_Close(file);
    SDL_EMFS_Destroy(context);
    return 0;
}
