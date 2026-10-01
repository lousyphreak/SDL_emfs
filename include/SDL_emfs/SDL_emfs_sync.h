// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SDL_emfs_sync_h_
#define SDL_emfs_sync_h_

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <SDL_emfs/SDL_emfs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_EMFS_File SDL_EMFS_File;

extern bool SDLCALL SDL_EMFS_WaitReady(SDL_EMFS_Context *context);
extern bool SDLCALL SDL_EMFS_GetPathInfo(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, SDL_PathInfo *info);
extern bool SDLCALL SDL_EMFS_EnumerateDirectory(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, SDL_EnumerateDirectoryCallback callback, void *userdata);
extern Uint64 SDLCALL SDL_EMFS_GetUserStorageSpaceRemaining(SDL_EMFS_Context *context);
extern bool SDLCALL SDL_EMFS_SetUserPathTime(SDL_EMFS_Context *context, const char *path, SDL_Time time);

extern SDL_EMFS_File *SDLCALL SDL_EMFS_Open(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, const char *mode);
extern SDL_IOStream *SDLCALL SDL_EMFS_OpenIO(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, const char *mode);
extern Sint64 SDLCALL SDL_EMFS_GetSize(SDL_EMFS_File *file);
extern Sint64 SDLCALL SDL_EMFS_Tell(SDL_EMFS_File *file);
extern Sint64 SDLCALL SDL_EMFS_Seek(SDL_EMFS_File *file, Sint64 offset, SDL_IOWhence whence);
extern SDL_IOStatus SDLCALL SDL_EMFS_GetStatus(SDL_EMFS_File *file);
extern size_t SDLCALL SDL_EMFS_Read(SDL_EMFS_File *file, void *destination, size_t length);
extern size_t SDLCALL SDL_EMFS_Write(SDL_EMFS_File *file, const void *source, size_t length);
extern size_t SDLCALL SDL_EMFS_ReadAt(SDL_EMFS_File *file, Uint64 offset, void *destination, size_t length, SDL_IOStatus *status);
extern bool SDLCALL SDL_EMFS_Flush(SDL_EMFS_File *file);
extern bool SDLCALL SDL_EMFS_Close(SDL_EMFS_File *file);
extern void SDLCALL SDL_EMFS_Prefetch(SDL_EMFS_File *file, Uint64 offset, Uint64 length);

extern void *SDLCALL SDL_EMFS_LoadFile(SDL_EMFS_Context *context, SDL_EMFS_Root root, const char *path, size_t *size);
extern bool SDLCALL SDL_EMFS_SaveUserFile(SDL_EMFS_Context *context, const char *path, const void *data, size_t size);
extern bool SDLCALL SDL_EMFS_CreateUserDirectory(SDL_EMFS_Context *context, const char *path);
extern bool SDLCALL SDL_EMFS_RemoveUserPath(SDL_EMFS_Context *context, const char *path);
extern bool SDLCALL SDL_EMFS_RenameUserPath(SDL_EMFS_Context *context, const char *old_path, const char *new_path);

#ifdef __cplusplus
}
#endif

#endif /* SDL_emfs_sync_h_ */
