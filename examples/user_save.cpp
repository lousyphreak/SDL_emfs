// SPDX-License-Identifier: GPL-3.0-or-later

#include <SDL3/SDL.h>
#include <SDL_emfs/SDL_emfs_sync.h>

int main(int argc, char **argv)
{
    SDL_EMFS_Config config;
    SDL_EMFS_InitConfig(&config);
    config.asset_root = argc > 1 ? argv[1] : ".";
    config.organization = "SDL_emfs";
    config.application = "user-save-example";
    SDL_EMFS_Context *context = SDL_EMFS_Create(&config);
    if (!context || !SDL_EMFS_WaitReady(context)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        return 1;
    }
    static const char contents[] = "persistent user data\n";
    if (!SDL_EMFS_SaveUserFile(context, "saves/example.txt", contents, sizeof(contents) - 1)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        return 1;
    }
    SDL_EMFS_Destroy(context);
    return 0;
}
