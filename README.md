# SDL_emfs

SDL_emfs is a small SDL3 filesystem library for games that use the same two
logical roots on desktop and the web:

- an immutable asset root; and
- a mutable user root below `SDL_GetPrefPath`.

It exposes synchronous seekable files, direct `SDL_IOStream` access, and a
callback-based asynchronous API driven by `SDL_EMFS_Pump`. Desktop operations
resolve a path and then stay close to SDL's native filesystem and AsyncIO APIs.
Emscripten asset reads use HTTP ranges with memory and IndexedDB chunk caches,
while user files use IDBFS and wait for persistence on flush, writable close,
save, and path mutations.

## Build

SDL 3.2.0 or newer is required. If `SDL3::SDL3` already exists, SDL_emfs uses
it; otherwise it calls `find_package(SDL3 REQUIRED CONFIG)` on Emscripten and
`find_package(SDL3 3.2 REQUIRED CONFIG)` on native platforms. The supplied web
SDL target must also provide SDL 3.2 or newer.

```cmake
add_subdirectory(SDL_emfs)
target_link_libraries(game PRIVATE SDL_emfs::SDL_emfs)
```

There is one static target containing both public fronts. On Emscripten the
target supplies the JavaScript library and legacy IDBFS link requirements. It
does not add Asyncify, pthread, LTO, or deployment flags. Applications that
call blocking SDL_emfs functions on the web must enable Asyncify themselves.
The async front does not require Asyncify.

Set `SDL_EMFS_BUILD_EXAMPLES=ON` to build the examples.

The examples cover direct `SDL_IOStream` access, opaque positional reads,
durable user saves, callback-based asynchronous reads, and an application-side
C++20 `co_await` adapter. The asynchronous examples use SDL's main-callback
API, so the same source drives Pump correctly on native platforms and
Emscripten without platform conditionals. Asyncify is enabled only on the
blocking example executables, never by the library target.

## Initialization

```c
#include <SDL_emfs/SDL_emfs_sync.h>

SDL_EMFS_Config config;
SDL_EMFS_InitConfig(&config);
config.asset_root = "assets";       /* directory natively, base URL on web */
config.organization = "My Studio"; /* may be NULL */
config.application = "My Game";

SDL_EMFS_Context *context = SDL_EMFS_Create(&config);
if (!context || !SDL_EMFS_WaitReady(context)) {
    SDL_Log("%s", SDL_GetError());
}
```

Async-only applications include `SDL_emfs_async.h`, call
`SDL_EMFS_ReadyAsync`, and pump until its callback is delivered. Native
readiness completes inline. Web readiness completes after both the asset
manifest and initial IDBFS population succeed.

Only one context may be created during a process or page lifetime. Filesystem
calls require successful readiness. Close all handles and finish foreground
work before destroying the context.

## Roots and paths

Public paths are UTF-8 and root-relative. SDL_emfs accepts `/` and `\\`, removes
`.` and repeated separators, resolves `..` without allowing it to cross the
selected root, and lowercases ASCII path bytes. Native startup maps these
logical lowercase paths to actual-case asset and existing user paths.

Asset-root operations never inspect user storage. User read-only open and load
first check physical user storage, then fall back to the asset root. User stat,
enumeration, remove, and rename inspect only user storage. Preserving user
write modes (`r+`, `a`, and their SDL variants) copy an asset fallback into
user storage before opening; truncating `w` modes start a fresh user file.

## Synchronous I/O

`SDL_EMFS_OpenIO` returns a direct SDL file stream on desktop after path
resolution. `SDL_EMFS_Open` adds an opaque cursor and positional
`SDL_EMFS_ReadAt`; the positional operation preserves the sequential cursor.

```c
SDL_EMFS_File *file = SDL_EMFS_Open(context, SDL_EMFS_ROOT_ASSET,
                                    "maps/world.bin", "rb");
SDL_IOStatus status;
size_t got = SDL_EMFS_ReadAt(file, offset, buffer, sizeof(buffer), &status);
SDL_EMFS_Close(file);
```

`SDL_EMFS_LoadFile` follows `SDL_LoadFile` ownership: the result is allocated
by SDL, contains an extra trailing NUL not included in the returned size, and
is released with `SDL_free`.

On the web, whole-file loads share the stream's bounded chunk cache. A cold
file uses one HTTP request; a partially cached file requests only its missing
ranges. Disk hits become memory-resident, and cached chunks copy directly into
the returned buffer. Cached chunks own chunk-sized buffers, so retaining one
chunk cannot retain the backing allocation of a large HTTP response. The
caller should still use seekable streams for archives and media rather than
whole-file loading them.

## Asynchronous I/O

Async modes are exactly `r`, `w`, `r+`, and `w+`. One handle may have only one
transfer or close pending. Different handles can progress independently.
Callbacks may complete inline or through the application's single Pump owner.
Callbacks are required. Failure to allocate an operation record or submit a
transfer/close task terminates the process. Open/load and completed I/O failures
are reported through callbacks. Callers must
sequence conflicting operations on the same physical user path.
Buffers passed to transfer and save calls remain caller-owned until callback.

```c
SDL_EMFS_ReadAtAsync(file, offset, buffer, length, ReadFinished, userdata);

/* once per frame, on one application thread */
SDL_EMFS_Pump(context);
```

A successful web write callback means the bytes reached the IDBFS working
filesystem. A successful writable close includes a completed `syncfs(false)`
and is the durability boundary.

`SDL_EMFS_SetUserPathTime` sets access and modification times at whole-second
precision and persists the web working filesystem.
`SDL_EMFS_GetUserStorageSpaceRemaining` forwards SDL file-storage reporting;
the generic SDL backend returns `SDL_MAX_UINT64` for an unknown amount, so it
is not a filesystem free-space or browser quota query.

## Wii U native assets

Wii U scans use the metadata returned by `FSReadDir` to build the same asset
and user-path indexes as other native platforms. They do not issue a separate
pathname stat for each directory entry. Directory recursion retains one native
client and separate directory handles. Asset and user APIs are unchanged;
mutable user-path queries still consult the filesystem.

The immutable asset index retains Cafe FS's native directory metadata. Native
asset streams use `FSAOpenFileByStat` to reuse that lookup instead of searching
the directory by pathname on each open. The FSA facade returns unavailable or
unsupported fast-open errors directly; these fall back to ordinary native open
on the same client, retaining the stream cache and read-ahead. The higher-level
FS facade can enter a permanent fatal-state wait for an unavailable fast-open,
so asset streams consistently use FSA for opening, reading and closing.
This is confined to the existing
immutable-asset contract: asset contents and directory entries must remain stable
while the context is in use. Mutable user files continue through SDL's ordinary
filesystem APIs.

Streams retain two aligned 64 KiB regions with asynchronous read-ahead. Bulk
reads use the caller's buffer and size when aligned; unaligned reads bounce in
batches up to 1 MiB. Close drains read-ahead before releasing native resources.
Async opens, positioned reads and whole-file loads use the same indexed native
open, with completions delivered by `SDL_EMFS_Pump`. Native callbacks do not run
synchronous file cleanup on the AppIO thread.

Synthesized directory metadata without a native filesystem cookie, such as
Aroma's redirected content metadata, uses ordinary SDL streams and AsyncIO.
No SDL or SDL_emfs public API changes, application hints or path-specific rules
are required.

## Web asset server

The web asset root must expose `.sdl-emfs/manifest.bin`, lowercase URL mapping,
byte ranges, stable bytes, ETags, and the required CORS headers. The included
development server supplies this contract:

```sh
python3 tools/emscripten_asset_server.py path/to/assets --port 8000
```

It emits COOP and COEP headers by default for pthread deployments. Use
`--no-isolation` for a simple single-threaded development deployment. Production
pthread builds also require SharedArrayBuffer support, cross-origin isolation,
correct worker MIME/hosting behavior, and compatible headers on every embedded
cross-origin resource.

The browser cache identity is the organization/application namespace plus
logical path, chunk size, version token, and chunk index. Asset caching is
best-effort: IndexedDB failures warn and valid HTTP data remains usable. There
is no cancellation, retry, timeout, quota API, transactional save, or final
sync during context destruction.

The complete API and behavioral contract is documented in
[docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).

## License

Copyright (C) 2026 SDL_emfs contributors.

SDL_emfs is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

SDL_emfs is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE. See [LICENSE](LICENSE) for the full license text.
