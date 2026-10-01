// SPDX-License-Identifier: GPL-3.0-or-later

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <SDL_emfs/SDL_emfs_async.h>

#include <coroutine>
#include <cstdlib>

class Task
{
public:
    struct promise_type;
    using Handle = std::coroutine_handle<promise_type>;

    struct promise_type
    {
        Task get_return_object() { return Task(Handle::from_promise(*this)); }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::abort(); }
    };

    Task() = default;
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    Task(Task &&other) noexcept : handle(other.handle)
    {
        other.handle = nullptr;
    }

    Task &operator=(Task &&other) noexcept
    {
        if (handle) {
            handle.destroy();
        }
        handle = other.handle;
        other.handle = nullptr;
        return *this;
    }

    ~Task()
    {
        if (handle) {
            handle.destroy();
        }
    }

private:
    explicit Task(Handle coroutine) : handle(coroutine) {}

    Handle handle = nullptr;
};

struct AwaitState
{
    void Begin(std::coroutine_handle<> handle)
    {
        continuation = handle;
        submitting = true;
        completed = false;
    }

    bool End()
    {
        submitting = false;
        return !completed;
    }

    void Complete()
    {
        completed = true;
        if (!submitting) {
            continuation.resume();
        }
    }

    std::coroutine_handle<> continuation;
    bool submitting = false;
    bool completed = false;
};

struct ReadyAwaiter
{
    explicit ReadyAwaiter(SDL_EMFS_Context *context) : context(context) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        state.Begin(handle);
        SDL_EMFS_ReadyAsync(context, Completed, this);
        return state.End();
    }

    bool await_resume() const noexcept { return success; }

    static void Completed(void *userdata, bool success)
    {
        ReadyAwaiter *awaiter = static_cast<ReadyAwaiter *>(userdata);
        awaiter->success = success;
        awaiter->state.Complete();
    }

    SDL_EMFS_Context *context;
    AwaitState state;
    bool success = false;
};

struct OpenAwaiter
{
    OpenAwaiter(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, const char *mode)
        : context(context), root(root), path(path), mode(mode) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        state.Begin(handle);
        SDL_EMFS_OpenAsync(context, root, path, mode, Completed, this);
        return state.End();
    }

    SDL_EMFS_AsyncFile *await_resume() const noexcept { return file; }

    static void Completed(void *userdata, SDL_EMFS_AsyncFile *file)
    {
        OpenAwaiter *awaiter = static_cast<OpenAwaiter *>(userdata);
        awaiter->file = file;
        awaiter->state.Complete();
    }

    SDL_EMFS_Context *context;
    SDL_EMFS_Root root;
    const char *path;
    const char *mode;
    AwaitState state;
    SDL_EMFS_AsyncFile *file = nullptr;
};

struct Transfer
{
    SDL_AsyncIOResult result;
    Uint64 bytes_transferred;
};

struct ReadAwaiter
{
    ReadAwaiter(SDL_EMFS_AsyncFile *file, Uint64 offset, void *destination, Uint64 length)
        : file(file), offset(offset), destination(destination), length(length) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        state.Begin(handle);
        SDL_EMFS_ReadAtAsync(file, offset, destination, length, Completed, this);
        return state.End();
    }

    Transfer await_resume() const noexcept { return transfer; }

    static void Completed(void *userdata, SDL_AsyncIOResult result, Uint64 bytes_transferred)
    {
        ReadAwaiter *awaiter = static_cast<ReadAwaiter *>(userdata);
        awaiter->transfer = { result, bytes_transferred };
        awaiter->state.Complete();
    }

    SDL_EMFS_AsyncFile *file;
    Uint64 offset;
    void *destination;
    Uint64 length;
    AwaitState state;
    Transfer transfer{ SDL_ASYNCIO_FAILURE, 0 };
};

struct CloseAwaiter
{
    explicit CloseAwaiter(SDL_EMFS_AsyncFile *file) : file(file) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        state.Begin(handle);
        SDL_EMFS_CloseAsync(file, Completed, this);
        return state.End();
    }

    bool await_resume() const noexcept { return success; }

    static void Completed(void *userdata, bool success)
    {
        CloseAwaiter *awaiter = static_cast<CloseAwaiter *>(userdata);
        awaiter->success = success;
        awaiter->state.Complete();
    }

    SDL_EMFS_AsyncFile *file;
    AwaitState state;
    bool success = false;
};

struct State
{
    SDL_EMFS_Context *context;
    const char *asset_path;
    Task task;
    bool done;
    bool success;
    Uint8 bytes[16];
};

static Task ReadAsset(State *state)
{
    if (!co_await ReadyAwaiter{ state->context }) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        state->success = false;
        state->done = true;
        co_return;
    }

    SDL_EMFS_AsyncFile *file = co_await OpenAwaiter{
        state->context, SDL_EMFS_ROOT_ASSET, state->asset_path, "r"
    };
    if (!file) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        state->success = false;
        state->done = true;
        co_return;
    }

    const Transfer transfer = co_await ReadAwaiter{
        file, 0, state->bytes, sizeof(state->bytes)
    };
    if (transfer.result != SDL_ASYNCIO_COMPLETE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        state->success = false;
    } else {
        SDL_Log("co_await read %" SDL_PRIu64 " bytes", transfer.bytes_transferred);
    }

    if (!co_await CloseAwaiter{ file }) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        state->success = false;
    }
    state->done = true;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
    if (!SDL_Init(0)) {
        return SDL_APP_FAILURE;
    }

    State *state = new State{};
    *appstate = state;
    state->asset_path = argc > 2 ? argv[2] : "sdl_emfs_async_await.js";
    state->success = true;

    SDL_EMFS_Config config;
    SDL_EMFS_InitConfig(&config);
    config.asset_root = argc > 1 ? argv[1] : ".";
    config.application = "async-await-example";
    state->context = SDL_EMFS_Create(&config);
    if (!state->context) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    state->task = ReadAsset(state);
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
