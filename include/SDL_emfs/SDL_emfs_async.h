// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SDL_emfs_async_h_
#define SDL_emfs_async_h_

#include <SDL3/SDL_asyncio.h>

#include <SDL_emfs/SDL_emfs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_EMFS_AsyncFile SDL_EMFS_AsyncFile;

typedef void (SDLCALL *SDL_EMFS_ReadyCallback)(void *userdata, bool success);
typedef void (SDLCALL *SDL_EMFS_OpenCallback)(void *userdata, SDL_EMFS_AsyncFile *file);
typedef void (SDLCALL *SDL_EMFS_TransferCallback)(void *userdata, SDL_AsyncIOResult result, Uint64 bytes_transferred);
typedef void (SDLCALL *SDL_EMFS_LoadCallback)(void *userdata, SDL_AsyncIOResult result, void *buffer, size_t size);
typedef void (SDLCALL *SDL_EMFS_CompletionCallback)(void *userdata, bool success);

extern void SDLCALL SDL_EMFS_ReadyAsync(SDL_EMFS_Context *context, SDL_EMFS_ReadyCallback callback, void *userdata);
extern void SDLCALL SDL_EMFS_OpenAsync(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, const char *mode, SDL_EMFS_OpenCallback callback, void *userdata);
extern Sint64 SDLCALL SDL_EMFS_GetAsyncFileSize(SDL_EMFS_AsyncFile *file);
extern void SDLCALL SDL_EMFS_ReadAtAsync(SDL_EMFS_AsyncFile *file, Uint64 offset, void *destination, Uint64 length, SDL_EMFS_TransferCallback callback, void *userdata);
extern void SDLCALL SDL_EMFS_WriteAtAsync(SDL_EMFS_AsyncFile *file, Uint64 offset, const void *source, Uint64 length, SDL_EMFS_TransferCallback callback, void *userdata);
extern void SDLCALL SDL_EMFS_CloseAsync(SDL_EMFS_AsyncFile *file, SDL_EMFS_CompletionCallback callback, void *userdata);
extern void SDLCALL SDL_EMFS_LoadFileAsync(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, SDL_EMFS_LoadCallback callback, void *userdata);
extern void SDLCALL SDL_EMFS_SaveUserFileAsync(SDL_EMFS_Context *context, const char *path, const void *data, size_t size, SDL_EMFS_CompletionCallback callback, void *userdata);
extern void SDLCALL SDL_EMFS_CreateUserDirectoryAsync(SDL_EMFS_Context *context, const char *path, SDL_EMFS_CompletionCallback callback, void *userdata);
extern void SDLCALL SDL_EMFS_RemoveUserPathAsync(SDL_EMFS_Context *context, const char *path, SDL_EMFS_CompletionCallback callback, void *userdata);
extern void SDLCALL SDL_EMFS_RenameUserPathAsync(SDL_EMFS_Context *context, const char *old_path, const char *new_path, SDL_EMFS_CompletionCallback callback, void *userdata);
extern void SDLCALL SDL_EMFS_Pump(SDL_EMFS_Context *context);

#ifdef __cplusplus
}
#endif

#endif /* SDL_emfs_async_h_ */
