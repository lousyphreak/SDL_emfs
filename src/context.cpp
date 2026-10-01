// SPDX-License-Identifier: GPL-3.0-or-later

#include "SDL_emfs_internal.h"

#include <new>

namespace {

bool context_created;

} // namespace

void SDL_EMFS_InitConfig(SDL_EMFS_Config *config)
{
    if (!config) {
        return;
    }
    SDL_zerop(config);
    config->chunk_size = 512 * 1024;
    config->prefetch_chunks = 1;
    config->memory_cache_chunks = 8;
}

SDL_EMFS_Context *SDL_EMFS_Create(const SDL_EMFS_Config *config)
{
    if (!config) {
        SDL_InvalidParamError("config");
        return nullptr;
    }
    if (!config->asset_root || !*config->asset_root) {
        SDL_InvalidParamError("config->asset_root");
        return nullptr;
    }
    if (!config->application || !*config->application) {
        SDL_InvalidParamError("config->application");
        return nullptr;
    }
    if (!config->chunk_size || (config->chunk_size & (config->chunk_size - 1))) {
        SDL_SetError("SDL_emfs chunk size must be a nonzero power of two");
        return nullptr;
    }
    if (context_created) {
        SDL_SetError("Only one SDL_emfs context may be created");
        return nullptr;
    }

    SDL_EMFS_Context *context = new (std::nothrow) SDL_EMFS_Context;
    if (!context) {
        SDL_OutOfMemory();
        return nullptr;
    }

    context->asset_root = config->asset_root;
    context->organization = config->organization ? config->organization : "";
    context->application = config->application;
    context->chunk_size = config->chunk_size;
    context->prefetch_chunks = config->prefetch_chunks;
    context->memory_cache_chunks = config->memory_cache_chunks;

    char *user_root = SDL_GetPrefPath(config->organization, config->application);
    if (!user_root) {
        delete context;
        return nullptr;
    }
    context->user_root = user_root;
    SDL_free(user_root);
    context_created = true;

#ifdef __EMSCRIPTEN__
    context->completion_lock = SDL_CreateMutex();
    if (!context->completion_lock) {
        delete context;
        return nullptr;
    }
    SDL_EMFS_WebStartInitialization(context);
#else
    context->async_queue = SDL_CreateAsyncIOQueue();
    if (!context->async_queue) {
        context->initialization_error = SDL_GetError();
        context->initialization_complete = true;
        return context;
    }
    context->ready = SDL_EMFS_BuildIndexes(context);
    if (!context->ready) {
        context->initialization_error = SDL_GetError();
    }
    context->initialization_complete = true;
#endif
    return context;
}

void SDL_EMFS_Destroy(SDL_EMFS_Context *context)
{
    if (!context) {
        return;
    }
    SDL_assert(context->initialization_complete);
#ifndef __EMSCRIPTEN__
    if (context->async_queue) {
        SDL_DestroyAsyncIOQueue(context->async_queue);
    }
#else
    SDL_assert(context->completions.empty());
    SDL_DestroyMutex(context->completion_lock);
#endif
    delete context;
}

bool SDL_EMFS_WaitReady(SDL_EMFS_Context *context)
{
    if (!context) {
        return SDL_InvalidParamError("context");
    }
#ifdef __EMSCRIPTEN__
    if (!context->initialization_complete && !SDL_EMFS_WebWaitReady(context)) {
        return false;
    }
#endif
    if (!context->ready) {
        return SDL_SetError("%s", context->initialization_error.c_str());
    }
    return true;
}
