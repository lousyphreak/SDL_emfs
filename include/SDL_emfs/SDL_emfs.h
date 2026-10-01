// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SDL_emfs_h_
#define SDL_emfs_h_

#include <SDL3/SDL_stdinc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_EMFS_Context SDL_EMFS_Context;

typedef enum SDL_EMFS_Root
{
    SDL_EMFS_ROOT_ASSET = 0,
    SDL_EMFS_ROOT_USER
} SDL_EMFS_Root;

typedef struct SDL_EMFS_Config
{
    const char *asset_root;
    const char *organization;
    const char *application;
    size_t chunk_size;
    Uint32 prefetch_chunks;
    Uint32 memory_cache_chunks;
} SDL_EMFS_Config;

extern void SDLCALL SDL_EMFS_InitConfig(SDL_EMFS_Config *config);
extern SDL_EMFS_Context *SDLCALL SDL_EMFS_Create(const SDL_EMFS_Config *config);
extern void SDLCALL SDL_EMFS_Destroy(SDL_EMFS_Context *context);

#ifdef __cplusplus
}
#endif

#endif /* SDL_emfs_h_ */
