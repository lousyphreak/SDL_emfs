// SPDX-License-Identifier: GPL-3.0-or-later

#include <SDL3/SDL.h>
#include <SDL_emfs/SDL_emfs_sync.h>

int main(int argc, char **argv)
{
    SDL_EMFS_Config config;
    SDL_EMFS_InitConfig(&config);
    config.asset_root = argc > 1 ? argv[1] : ".";
    config.application = "sync-iostream-example";

    SDL_EMFS_Context *context = SDL_EMFS_Create(&config);
    if (!context || !SDL_EMFS_WaitReady(context)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        return 1;
    }
    const char *asset_path = argc > 2 ? argv[2] : "sdl_emfs_sync_iostream.js";
    SDL_IOStream *stream = SDL_EMFS_OpenIO(context, SDL_EMFS_ROOT_ASSET, asset_path, "rb");
    if (!stream) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        return 1;
    }
    SDL_Log("asset size: %" SDL_PRIs64, SDL_GetIOSize(stream));
    SDL_CloseIO(stream);
    SDL_EMFS_Destroy(context);
    return 0;
}
