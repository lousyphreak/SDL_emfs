// SPDX-License-Identifier: GPL-3.0-or-later

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <SDL_emfs/SDL_emfs_async.h>

struct State
{
    SDL_EMFS_Context *context;
    SDL_EMFS_AsyncFile *file;
    const char *asset_path;
    bool done;
    bool success;
    Uint8 bytes[16];
};

static void Closed(void *userdata, bool success)
{
    State *state = static_cast<State *>(userdata);
    if (!success) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        state->success = false;
    }
    state->file = nullptr;
    state->done = true;
}

static void Read(void *userdata, SDL_AsyncIOResult result, Uint64 bytes_transferred)
{
    State *state = static_cast<State *>(userdata);
    if (result != SDL_ASYNCIO_COMPLETE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        state->success = false;
    } else {
        SDL_Log("read %" SDL_PRIu64 " bytes", bytes_transferred);
    }
    SDL_EMFS_CloseAsync(state->file, Closed, state);
}

static void Opened(void *userdata, SDL_EMFS_AsyncFile *file)
{
    State *state = static_cast<State *>(userdata);
    if (!file) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        state->success = false;
        state->done = true;
        return;
    }
    state->file = file;
    SDL_EMFS_ReadAtAsync(file, 0, state->bytes, sizeof(state->bytes), Read, state);
}

static void Ready(void *userdata, bool success)
{
    State *state = static_cast<State *>(userdata);
    if (!success) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        state->success = false;
        state->done = true;
        return;
    }
    SDL_EMFS_OpenAsync(state->context, SDL_EMFS_ROOT_ASSET, state->asset_path, "r", Opened, state);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
    if (!SDL_Init(0)) {
        return SDL_APP_FAILURE;
    }

    State *state = new State{};
    *appstate = state;
    state->asset_path = argc > 2 ? argv[2] : "sdl_emfs_async_callbacks.js";
    state->success = true;

    SDL_EMFS_Config config;
    SDL_EMFS_InitConfig(&config);
    config.asset_root = argc > 1 ? argv[1] : ".";
    config.application = "async-callback-example";
    state->context = SDL_EMFS_Create(&config);
    if (!state->context) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_EMFS_ReadyAsync(state->context, Ready, state);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    State *state = static_cast<State *>(appstate);
    SDL_EMFS_Pump(state->context);
    if (!state->done) {
        return SDL_APP_CONTINUE;
    }
    return state->success ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppEvent(void *, SDL_Event *)
{
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult)
{
    State *state = static_cast<State *>(appstate);
    if (state) {
        SDL_EMFS_Destroy(state->context);
        delete state;
    }
    SDL_Quit();
}
