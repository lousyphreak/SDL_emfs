# SDL_emfs implementation plan

## Outcome

Build one small static SDL3 filesystem library with two explicit roots and two
public I/O fronts.

The asset root is immutable. Native builds read it directly through SDL file
APIs. Emscripten builds read it from a server through HTTP range requests and
normal whole-file requests, backed by a bounded session chunk cache and a
best-effort persistent IndexedDB chunk cache.

The user root is mutable. Native builds place it below `SDL_GetPrefPath` and
use SDL file APIs directly. Emscripten mounts the same logical root in IDBFS,
populates it before the context becomes ready, and calls `FS.syncfs(false)`
when a writable file is flushed or closed and after whole-file saves or path
mutations.

The roots remain physically and semantically separate:

- asset-root operations never inspect user storage;
- user-root read-only open and whole-file load first inspect user storage and
  fall back to the asset root when the physical user path is absent;
- user-root path information, enumeration, remove, and rename inspect only
  physical user storage; and
- preserving writable user opens materialize an asset fallback into user
  storage before opening it, while truncating modes create a fresh user file.

The synchronous front follows `SDL_IOStream` conventions and also exposes an
opaque file handle with positional `ReadAt`. The asynchronous front follows
the shape of SDL AsyncIO but delivers callbacks through `SDL_EMFS_Pump` for
work that does not complete inline. Sync and async file handles are distinct
and cannot cross between fronts.

The library always contains both fronts. On Emscripten, the consuming
application must enable Asyncify if it calls blocking APIs. Calling a blocking
web API without Asyncify is application error and has undefined behavior. The
async front does not require Asyncify.

## Design priorities

1. Keep native steady-state I/O as close as possible to direct SDL calls.
2. Keep the Emscripten byte path bounded, cache-aware, and free of unnecessary
   C++/JavaScript duplicate representations.
3. Mirror SDL names, return conventions, status values, mode strings,
   ownership, and error reporting where a direct SDL counterpart exists.
4. Prefer caller-owned sequencing over library mutexes. One file handle has at
   most one operation in progress; callers open more handles for parallelism.
5. Keep persistent user-storage behavior appropriate for ordinary game saves,
   not database or transactional workloads.
6. Keep implementation state direct. Do not add backend class hierarchies,
   mount stacks, cancellation systems, retry policies, compatibility layers,
   or speculative extension points.

Debug assertions may diagnose the two central usage invariants: the context
must be ready before filesystem calls, and a file handle may have only one
operation in progress. Release builds do not guard caller misuse.

## Scope

### In scope

- one configured immutable asset root;
- one SDL preference-path mutable user root;
- synchronous `SDL_IOStream` access;
- a synchronous opaque file API with seek, sequential transfer, and `ReadAt`;
- an asynchronous opaque file API with positional transfer and pumped
  callbacks;
- path information through `SDL_PathInfo`;
- SDL-shaped directory enumeration;
- whole-file load with `SDL_LoadFile` allocation semantics;
- whole-file user save;
- recursive user-directory creation, non-recursive remove, and rename;
- user-first read fallback to assets and copy-on-write for preserving user
  write modes;
- lowercase logical paths backed by actual-case native/server maps;
- HTTP Range reads, foreground and explicit prefetch, and `200` fallback;
- a bounded Emscripten memory LRU;
- persistent immutable asset chunks in direct IndexedDB records;
- legacy Emscripten FS plus IDBFS for mutable user files;
- single-threaded and pthread Emscripten applications;
- C and C++ consumers; and
- an application-side example showing how callbacks can be adapted to C++20
  `co_await` without shipping a task or awaiter type in the library.

### Deliberately out of scope

- a general mount/provider/backend system;
- archive parsing, decompression, or archive-subrange objects;
- stdio, PhysFS, engine file-class, or job-system compatibility layers;
- glob syntax and search-path policy;
- public C++ wrappers;
- handle duplication;
- more than one live context;
- context recreation after destruction;
- async path information or directory enumeration;
- async flush, async prefetch, async destruction, or explicit user-data sync;
- filesystem free-space and browser quota queries;
- cancellation, timeout, or automatic HTTP retry;
- atomic save replacement, whiteouts, or transactional user storage;
- persistent-cache size management or old-namespace cleanup;
- authenticated/custom-header asset Fetch configuration;
- arbitrary URL templates or asset-root query strings;
- support for non-UTF-8 native filenames;
- empty asset directories;
- runtime observation of externally created mixed-case user paths without a
  restart; and
- consuming-project integration work.

## Public headers and target

The project exports one static CMake target:

```text
SDL_emfs::SDL_emfs
```

It provides three C headers:

```text
include/SDL_emfs/SDL_emfs.h
include/SDL_emfs/SDL_emfs_sync.h
include/SDL_emfs/SDL_emfs_async.h
```

`SDL_emfs.h` contains configuration, the opaque context declaration, shared
types, and context lifetime. `SDL_emfs_sync.h` includes it and declares
blocking readiness, path operations, and synchronous file access.
`SDL_emfs_async.h` includes it and declares callback readiness, asynchronous
file/path operations, and Pump. Every public symbol uses the `SDL_EMFS_`
prefix.

Each header includes the SDL headers needed by its declarations and wraps its
C declarations in the normal `extern "C"` guard for C++ consumers.

The signatures below are the contract to implement. Minor formatting or
const-correctness corrections are permitted, but their return conventions,
ownership, and operation set are fixed.

## Shared C API and lifecycle

### Types and configuration

```c
typedef struct SDL_EMFS_Context SDL_EMFS_Context;

typedef enum SDL_EMFS_Root {
    SDL_EMFS_ROOT_ASSET = 0,
    SDL_EMFS_ROOT_USER
} SDL_EMFS_Root;

typedef struct SDL_EMFS_Config {
    const char *asset_root;
    const char *organization;
    const char *application;
    size_t chunk_size;
    Uint32 prefetch_chunks;
    Uint32 memory_cache_chunks;
} SDL_EMFS_Config;

void SDL_EMFS_InitConfig(SDL_EMFS_Config *config);
SDL_EMFS_Context *SDL_EMFS_Create(const SDL_EMFS_Config *config);
void SDL_EMFS_Destroy(SDL_EMFS_Context *context);
```

`SDL_EMFS_InitConfig` initializes the whole structure. It sets all pointer
fields to `NULL` and sets these numeric defaults:

- `chunk_size = 512 * 1024`;
- `prefetch_chunks = 1`; and
- `memory_cache_chunks = 8`.

The caller then supplies nonempty `asset_root`, nonempty `application`, and
optionally `organization`, following the nullable organization convention of
`SDL_GetPrefPath`. Chunk size must be a nonzero power of two. Configuration is
copied during creation, becomes immutable, and is used to precompute range
shift/mask values.

`prefetch_chunks = 0` disables automatic prefetch. `memory_cache_chunks = 0`
retains no ready chunks between operations; in-flight data and caller-owned
results still use memory. Neither setting disables the best-effort persistent
asset cache on Emscripten.

There is no config struct version field and no native user-root override.

### Initialization

`SDL_EMFS_Create` starts initialization. Filesystem calls are legal only after
one readiness operation succeeds. Invalid configuration or failure to allocate
the context makes `Create` return `NULL` with `SDL_GetError` set. Initialization
failures after creation are reported by `WaitReady` or `ReadyAsync`; the error
is available through `SDL_GetError` on the thread receiving that result.

Native creation synchronously scans the asset and existing user trees, so
`ReadyAsync` completes inline and `WaitReady` returns immediately.
Emscripten readiness waits for both the server manifest and initial
`FS.syncfs(true)` IDBFS population. A manifest or IDBFS failure fails the
entire context. Failure to open the best-effort immutable asset cache only
logs a warning and does not fail readiness.

`ReadyAsync` completion is delivered through Pump when initialization was
actually asynchronous. `WaitReady` uses the blocking web bridge and therefore
requires an Asyncify-enabled application on Emscripten.

Calling filesystem APIs before readiness is undefined in release builds and
asserts in debug builds.

Only one context may be created in a process/page. Destruction requires
initialization to have completed, successfully or unsuccessfully. After
successful readiness, it additionally requires an idle context and all opaque
and SDL streams to have been closed. Here, idle excludes detached prefetch and
asset-cache writes, which do not retain the C context. The library does not
track live handles. Destruction performs no final IDBFS sync, does not cancel
foreground work, and does not promise that another context can be created
later.

The Emscripten JavaScript cache service persists until page exit so
fire-and-forget prefetch and asset-cache writes can finish after the C context
is destroyed.

### Path information and enumeration

These blocking declarations live in `SDL_emfs_sync.h`:

```c
bool SDL_EMFS_GetPathInfo(SDL_EMFS_Context *context,
                          SDL_EMFS_Root root,
                          const char *path,
                          SDL_PathInfo *info);

bool SDL_EMFS_EnumerateDirectory(SDL_EMFS_Context *context,
                                 SDL_EMFS_Root root,
                                 const char *path,
                                 SDL_EnumerateDirectoryCallback callback,
                                 void *userdata);
```

These mirror SDL return and callback conventions and report details through
`SDL_GetError` on failure. They are synchronous memory/native-filesystem
operations after readiness and have no async variants.

Asset path information and enumeration use the immutable startup index. User
path information and enumeration inspect physical user storage only; they do
not apply asset fallback. User enumeration reads the live user tree and
returns lowercase logical names. The callback receives the normalized logical
directory with a trailing `/` (`/` for the logical root) as `dirname` and one
lowercase immediate name as `fname`. Case-only collisions produce one
unspecified entry. Enumeration order is unspecified.

An empty normalized path denotes the selected root for path information and
enumeration.

## Synchronous C API

```c
typedef struct SDL_EMFS_File SDL_EMFS_File;

bool SDL_EMFS_WaitReady(SDL_EMFS_Context *context);

SDL_EMFS_File *SDL_EMFS_Open(SDL_EMFS_Context *context,
                             SDL_EMFS_Root root,
                             const char *path,
                             const char *mode);

SDL_IOStream *SDL_EMFS_OpenIO(SDL_EMFS_Context *context,
                              SDL_EMFS_Root root,
                              const char *path,
                              const char *mode);

Sint64 SDL_EMFS_GetSize(SDL_EMFS_File *file);
Sint64 SDL_EMFS_Tell(SDL_EMFS_File *file);
Sint64 SDL_EMFS_Seek(SDL_EMFS_File *file,
                     Sint64 offset,
                     SDL_IOWhence whence);
SDL_IOStatus SDL_EMFS_GetStatus(SDL_EMFS_File *file);

size_t SDL_EMFS_Read(SDL_EMFS_File *file,
                     void *destination,
                     size_t length);
size_t SDL_EMFS_Write(SDL_EMFS_File *file,
                      const void *source,
                      size_t length);
size_t SDL_EMFS_ReadAt(SDL_EMFS_File *file,
                       Uint64 offset,
                       void *destination,
                       size_t length,
                       SDL_IOStatus *status);

bool SDL_EMFS_Flush(SDL_EMFS_File *file);
bool SDL_EMFS_Close(SDL_EMFS_File *file);

void SDL_EMFS_Prefetch(SDL_EMFS_File *file,
                       Uint64 offset,
                       Uint64 length);

void *SDL_EMFS_LoadFile(SDL_EMFS_Context *context,
                        SDL_EMFS_Root root,
                        const char *path,
                        size_t *size);

bool SDL_EMFS_SaveUserFile(SDL_EMFS_Context *context,
                           const char *path,
                           const void *data,
                           size_t size);

bool SDL_EMFS_CreateUserDirectory(SDL_EMFS_Context *context,
                                  const char *path);
bool SDL_EMFS_RemoveUserPath(SDL_EMFS_Context *context,
                             const char *path);
bool SDL_EMFS_RenameUserPath(SDL_EMFS_Context *context,
                             const char *old_path,
                             const char *new_path);
```

Opaque sync operations mirror `SDL_IOStream` return values and status rules.
`GetSize`, `Tell`, and `Seek` return negative values on failure. Reads and
writes return transferred byte counts. `ReadAt` requires its output
`SDL_IOStatus` pointer and does not modify the sequential status.

One handle may execute only one operation at a time. Concurrent calls on one
handle are caller error. Native `ReadAt` saves the current position, seeks,
reads, and restores the position on the same SDL stream. It introduces no
second stream and no mutex.

`OpenIO` returns a direct SDL file stream on native platforms after path
resolution. It is intentionally untracked. Emscripten returns a custom stream
for HTTP assets and for IDBFS user files that need persistent flush/close
behavior.

Asset opens accept only read-only mode strings. User sync opens accept SDL's
normal `SDL_IOFromFile` mode grammar. User read-only opens fall back to assets
without copying. When a physical user file is absent:

- `r+` variants load an asset fallback, create missing user parent
  directories, materialize a user copy, and then open it; they fail normally
  if neither root contains the file;
- `a` and `a+` variants copy an asset fallback before appending, but otherwise
  retain SDL's create-if-missing behavior; and
- `w` and `w+` variants create a fresh user file without downloading the
  asset. Their `x` variants retain SDL's exclusive-create check against the
  physical user path.

The initial copy-on-write materialization is not synchronized to IDBFS before
the handle is returned. Writable close performs persistence. If materializing
the copy succeeds but opening the new handle fails, the unsynchronized working
copy may remain; no rollback is attempted.

`SDL_EMFS_LoadFile` mirrors `SDL_LoadFile`: the returned SDL allocation has one
trailing NUL not included in `size` and is released with `SDL_free`. Asset
loads use the whole-file cache policy described below. User loads fall back to
assets without materializing a user copy.

`SDL_EMFS_SaveUserFile` creates missing parent directories, truncates/writes
the destination directly, closes it, and waits for web persistence. It does
not use temporary-file atomic replacement.

User directory creation recursively creates missing parents. Remove accepts a
file or empty directory and is not recursive. Rename follows
`SDL_RenamePath`, including replacement of an existing destination. Remove and
rename operate only on physical user entries. Removing an override may reveal
the immutable asset again; an asset-only source fails as read-only. No
whiteout/tombstone state exists.

`Prefetch` is fire-and-forget. It has no status or completion result and is a
no-op natively.

## Asynchronous C API

### Callback types

```c
typedef struct SDL_EMFS_AsyncFile SDL_EMFS_AsyncFile;

typedef void (*SDL_EMFS_ReadyCallback)(void *userdata, bool success);

typedef void (*SDL_EMFS_OpenCallback)(void *userdata,
                                      SDL_EMFS_AsyncFile *file);

typedef void (*SDL_EMFS_TransferCallback)(void *userdata,
                                          SDL_AsyncIOResult result,
                                          Uint64 bytes_transferred);

typedef void (*SDL_EMFS_LoadCallback)(void *userdata,
                                      SDL_AsyncIOResult result,
                                      void *buffer,
                                      size_t size);

typedef void (*SDL_EMFS_CompletionCallback)(void *userdata,
                                            bool success);
```

Open failure passes `NULL`. Non-transfer completion passes `false`. Transfer
and load callbacks use `SDL_AsyncIOResult`. On failure, `SDL_GetError` is valid
during the callback. The library captures asynchronous error text and restores
it on the callback thread immediately before delivery.

Failed load callbacks pass `NULL` and size zero. A transfer callback reports
the actual partial byte count, if any, for either a success or failure result.

### Operations

```c
void SDL_EMFS_ReadyAsync(SDL_EMFS_Context *context,
                         SDL_EMFS_ReadyCallback callback,
                         void *userdata);

void SDL_EMFS_OpenAsync(SDL_EMFS_Context *context,
                        SDL_EMFS_Root root,
                        const char *path,
                        const char *mode,
                        SDL_EMFS_OpenCallback callback,
                        void *userdata);

Sint64 SDL_EMFS_GetAsyncFileSize(SDL_EMFS_AsyncFile *file);

void SDL_EMFS_ReadAtAsync(SDL_EMFS_AsyncFile *file,
                          Uint64 offset,
                          void *destination,
                          Uint64 length,
                          SDL_EMFS_TransferCallback callback,
                          void *userdata);

void SDL_EMFS_WriteAtAsync(SDL_EMFS_AsyncFile *file,
                           Uint64 offset,
                           const void *source,
                           Uint64 length,
                           SDL_EMFS_TransferCallback callback,
                           void *userdata);

void SDL_EMFS_CloseAsync(SDL_EMFS_AsyncFile *file,
                         SDL_EMFS_CompletionCallback callback,
                         void *userdata);

void SDL_EMFS_LoadFileAsync(SDL_EMFS_Context *context,
                            SDL_EMFS_Root root,
                            const char *path,
                            SDL_EMFS_LoadCallback callback,
                            void *userdata);

void SDL_EMFS_SaveUserFileAsync(SDL_EMFS_Context *context,
                                const char *path,
                                const void *data,
                                size_t size,
                                SDL_EMFS_CompletionCallback callback,
                                void *userdata);

void SDL_EMFS_CreateUserDirectoryAsync(
    SDL_EMFS_Context *context,
    const char *path,
    SDL_EMFS_CompletionCallback callback,
    void *userdata);

void SDL_EMFS_RemoveUserPathAsync(
    SDL_EMFS_Context *context,
    const char *path,
    SDL_EMFS_CompletionCallback callback,
    void *userdata);

void SDL_EMFS_RenameUserPathAsync(
    SDL_EMFS_Context *context,
    const char *old_path,
    const char *new_path,
    SDL_EMFS_CompletionCallback callback,
    void *userdata);

void SDL_EMFS_Pump(SDL_EMFS_Context *context);
```

Async submission functions return `void`. Failure to allocate an operation
record or enqueue a transfer or close task is fatal and terminates the process.
SDL whole-file load submission failures are delivered as load callback failures;
ordinary open, network, storage, and I/O failures are also callback results.

Every async callback argument is required and must be non-`NULL`. Callbacks
may run before the submitting function returns, so callers must be prepared
for both inline and pumped completion.

The library copies async path strings before returning. The caller retains
read destinations and write/save sources until completion. Successful load
callbacks transfer an SDL allocation with a trailing NUL to the caller, who
frees it with `SDL_free`. Successful open callbacks transfer the file handle.

Async file modes are exactly SDL AsyncIO's `r`, `w`, `r+`, and `w+`; there is
no append or `b` mode. Assets accept only `r`. User `r+` performs copy-on-write
when only an asset exists; `w` and `w+` do not copy.

One async file has at most one pending operation. Submitting a second transfer
or close while busy is undefined and asserts in debug builds. Close is called
after the transfer callback, including directly from that callback. A
successful write callback means bytes reached the working filesystem; web
durability is promised only after successful writable close. Writable async
close always requests durable storage. Close destroys the handle even when
persistence fails, matching SDL close ownership behavior.

`GetAsyncFileSize` is a synchronous metadata query. It returns a negative
value and sets `SDL_GetError` on failure and must not race a pending operation
on the same handle.

Callbacks for pure memory hits may run inline on the submitting thread when
that is possible without extra state or duplicate lookup. Native path
mutations also run inline because their SDL counterparts are expected to be
short. Other callbacks are delivered by `SDL_EMFS_Pump`.

Async operations may be submitted from any native thread. One application
thread owns Pump and never pumps concurrently. Pump drains completions until
empty. Pumped callbacks may submit new operations and may close an idle
handle. Different files and handles may complete in any order.

Callers sequence conflicting operations on the same physical user path.
SDL_emfs adds no context-wide path lock; races between different handles or
path mutations have the outcome of the underlying filesystem.

No public coroutine wrapper ships initially. The async/await example defines
an application-side adapter. Its submit path performs one cache lookup: if the
operation completes synchronously, `await_suspend` declines suspension;
otherwise the callback resumes it through Pump. Destroying a suspended
application coroutine before completion is application error. The example
submits from the Pump-owning thread, so Pump cannot race `await_suspend` on a
second thread.

## Path contract

Public paths are valid UTF-8 and root-relative. Normalization:

1. treats both `/` and `\\` as separators;
2. removes repeated separators and `.` components;
3. applies `..` and rejects a result that would escape the selected root;
4. strips leading separators;
5. lowercases ASCII `A` through `Z` and leaves other UTF-8 bytes unchanged;
   and
6. preserves no alternate spelling after the lowercase logical key is built.

Leading separators are tolerated and stripped. An empty normalized path is
the logical root for stat and enumeration. Other empty-path behavior follows
the selected underlying operation and is caller responsibility.

The roots are naming and routing boundaries, not security sandboxes. Native
symlinks are followed according to SDL behavior, even when they escape a root.
Asset trees are trusted to contain no cyclic directory symlinks; startup
scanning performs no cycle detection. Non-UTF-8 POSIX names are not supported.

Case-fold collisions deliberately have no specified winner. Startup scanning
and the reference server build a direct lowercase-to-actual-path map and keep
one encountered entry. The public documentation does not promise which one.

## Native indexes and path resolution

`SDL_EMFS_Create` recursively scans the native asset root synchronously. The
asset index contains files only. It stores lowercase logical paths, actual
filesystem paths, size, and native SDL modification time. Directories are
inferred from file paths; empty asset directories are invisible.

Each inferred asset directory has size zero. Its modification time is the
newest immediate file child time, or zero if it has no immediate file child.
Child-directory times are not folded into the parent. Enumeration order is
unspecified.

The completed asset index is immutable and can be read concurrently without
locks.

Initialization also scans existing user entries into a
lowercase-to-actual-path map so older or externally produced mixed-case names
remain addressable. After readiness:

- existing mapped names use the actual spelling captured at startup;
- new SDL_emfs paths use the actual spelling of an existing mapped parent
  and lowercase spelling for new components;
- if a mapped path no longer exists, lookup tries existing mapped ancestors
  before falling back to the lowercase physical path;
- rename updates the affected map entries under a mutex shared with lookup; and
- mixed-case paths created externally after readiness are not guaranteed to
  resolve until the game restarts.

User enumeration reads the underlying directory every time, includes empty
directories, and returns lowercase names. User-created files and directories
are therefore visible in the same session without rescanning the case map.

## Native backend

### Synchronous files

- Resolve the logical path once at open.
- Return direct `SDL_IOStream` files from `OpenIO`.
- Back opaque sync files with one SDL stream.
- Delegate size, seek, tell, sequential read/write, status, flush, and close to
  SDL-shaped behavior.
- Implement `ReadAt` with save-position, seek, read, and restore on the same
  stream.
- Perform no implicit prefetch or caching.
- Use `SDL_GetPrefPath(organization, application)` for user storage.
- Use direct SDL filesystem mutations and `SDL_SaveFile`-shaped writes.

### Asynchronous files

- Create one `SDL_AsyncIOQueue` per context.
- Open with `SDL_AsyncIOFromFile` during submission, matching SDL's
  synchronous-open assumption.
- Submit one read/write task at a time per async file.
- Drain SDL outcomes from Pump and translate them to public callbacks.
- Close writable files with SDL's flush behavior enabled.
- Implement native async path mutations inline with SDL filesystem calls.
- Implement async whole-file loading with `SDL_LoadFileAsync`.
- Implement async whole-file saving as an SDL async open, write, and close
  sequence.
- Use no library worker or worker pool.

Parallelism comes from different handles. Multiple handles for the same asset
share only immutable path metadata natively; the operating system supplies the
filesystem cache.

## Server contract

Emscripten requires a server that maps normalized lowercase URLs to actual
asset paths and serves a binary manifest at:

```text
.sdl-emfs/manifest.bin
```

The repository ships a Python reference/development server. It builds its
lowercase map and manifest at startup. Asset-tree case collisions select one
unspecified file.

### Binary manifest

All integers are little-endian. The file starts with:

```text
8 bytes   magic/version: "SDLEMFS1"
u32       file_record_count
```

Each file record is:

```text
u64       byte_size
i64       modification_time_milliseconds
u32       lowercase_utf8_path_length
u32       etag_length
bytes     lowercase UTF-8 path
bytes     optional HTTP ETag, including its normal quoted syntax
```

Only files are recorded. Directories are inferred client-side. The web client
rejects a manifest file size above JavaScript's exact integer range (`2^53`).
Modification times become SDL nanoseconds at millisecond precision. Native
metadata retains the precision SDL supplies. Later HTTP `Last-Modified`
headers are neither parsed nor stored; the manifest remains the sole web mtime
source.

The sample server generates a strong ETag from file size and modification
time without hashing file contents. Deployments may supply content hashes,
weak ETags, constant ETags, or no ETag. Weak ETags participate in equality
change detection but are not sent in `If-Range`. Strong ETags may be sent in
`If-Range`. A response with no ETag uses an empty version token; same-size
changes can then remain undetected.

### HTTP behavior

Chunk acquisition requests:

```http
Range: bytes=<start>-<end>
```

A `206` must have a matching `Content-Range`, total size, and response length.
If the server ignores Range and returns `200`, the client treats the response
as the complete file, satisfies the foreground read, and writes every logical
chunk to persistent storage through the normal chunk path.

The first such `200` response is promoted to the file's in-flight full-body
result so newly arriving ranges join it. Range Fetches already started for
other chunks are not canceled; any duplicate full bodies they return are
discarded after validation instead of being cached repeatedly.

Byte responses use identity/stable bytes. Asset roots are clean base URLs with
no query or fragment. The client lowercases and percent-encodes individual
path components before joining them to the root. Assets are public/static;
SDL_emfs has no credentials or custom request-header callback.

The reference server:

- supports Range and If-Range;
- exposes `Content-Range` and `ETag`;
- permits cross-origin `Range` and `If-Range` headers;
- emits CORS headers needed by the client;
- emits COOP/COEP headers by default for pthread builds; and
- serves stable, untransformed bytes.

There is no library-managed request timeout or retry. A failed foreground
request completes with an I/O failure; a later application operation naturally
tries again. Speculative prefetch failures are silent.

## Emscripten asset index

Context creation starts one Fetch of the binary manifest. The response is
copied once into Wasm memory and parsed linearly into the same immutable C++
record and child-list representation built by native scanning. The transfer
buffer is then freed. JavaScript does not retain a second manifest tree.

Path information and enumeration are lock-free C++ index operations after
readiness, including in pthread builds.

The manifest is a startup snapshot. If a later byte response reports a
different size or ETag, SDL_emfs logs a warning, invalidates that file's asset
chunks, updates the affected open handle and direct asset-cache metadata, and
continues with the received bytes. The shared path-info index remains
unchanged; applications should restart to obtain a consistent new manifest.

## Emscripten asset cache

### Identity and stores

The asset cache uses direct IndexedDB and never IDBFS. Its database namespace
is derived from SDL `organization` and `application`. Asset and user storage
remain in separate IndexedDB mechanisms.

A chunk identity contains:

```text
application namespace
+ lowercase requested relative path
+ logical chunk size
+ version token (ETag or empty)
+ chunk index
```

The full URL is intentionally not part of the key. Applications that reuse an
organization/application namespace for a different asset server accept cache
aliasing unless ETags or explicit cache clearing distinguish the content.

Use one `chunks` object store with `{key, data}` records. The key contains the
complete cache identity; `data` is the chunk's owned `ArrayBuffer`. A separate
file-metadata store would duplicate manifest/observed metadata without serving
reads. Older databases may still contain an unused `files` store and redundant
chunk fields; their chunk records remain readable without a schema migration.
Path invalidation uses a cursor bounded to the serialized path's key prefix.

There is no 64 KiB part splitting, complete-file flag, chunk bitmap,
persistent LRU, quota manager, or previous-namespace cleanup.

### Session LRU and in-flight work

One JavaScript `Map` holds ready chunks in LRU order. Capacity is a
chunk count. Reads promote entries; insertion above the configured limit
evicts the least-recently-used ready chunk. In-flight Promises are separate and
cannot be evicted.

One Promise exists per missing chunk. Concurrent foreground reads and
prefetches for that chunk join it. One Promise also exists per in-flight full
GET so concurrent whole-file loads share one transfer. These maps run on the
browser JavaScript event loop and require no cache mutex.

If a full GET is in flight, a new range read for the same file joins it instead
of issuing another request. Range requests that were already in flight when a
full GET starts are allowed to finish independently.

### Chunk acquisition

`ensureChunk` follows one direct order:

1. Return a ready memory-LRU entry.
2. Join the exact in-flight chunk Promise.
3. Join an in-flight full GET for the file.
4. Read the one persistent IndexedDB chunk record.
5. On miss or corruption, delete the affected record and issue one Range
   request.
6. Validate the response, place the requested chunk in the memory LRU, and
   resolve foreground waiters.
7. Start an independent background IndexedDB write.
8. Remove the in-flight Promise when settled.

IndexedDB open/read/write/delete failure never turns valid HTTP bytes into an
I/O failure. Asset-cache initialization and background write failures produce
console warnings. Corrupt records are deleted individually. Foreground reads
never wait for persistent-cache writes.

Offsets cross the JS boundary as high/low `Uint32` words. JavaScript
reconstructs an exact Number after the manifest has established the below-
`2^53` file-size invariant. No Wasm BigInt ABI option is required.

### Range reads and prefetch

For a foreground range:

1. Clamp to the handle's current known size.
2. Compute the required logical chunks using the configured chunk size.
3. Start every missing foreground chunk without a library concurrency limit.
4. Start the configured following prefetch chunks at the same time, but do not
   include their Promises in foreground completion.
5. Await all foreground chunks.
6. Copy requested subranges into the current Wasm heap view.

The browser/network stack controls actual network concurrency. Automatic
prefetch applies to sequential and positional reads. Explicit `Prefetch`
enters the same path and has no result. Successfully prefetched chunks use the
same memory LRU and background IndexedDB persistence as foreground chunks.

### Whole-file load

`LoadFile[Async]` is the only whole-file API. It returns an SDL allocation with
a trailing NUL and does not create a seekable whole-file handle.

For an asset load:

1. Determine expected chunks from the handle/index size and version.
2. Read all expected records from the combined memory/IndexedDB cache.
3. Promote disk hits into the session LRU. If every chunk is present and valid,
   copy chunks directly into the SDL output and make no HTTP request or
   intermediate whole-file assembly buffer.
4. For partial coverage, keep cached chunks and acquire only missing ranges
   through the normal in-flight chunk path, without repeating known disk misses.
   If a response changes the asset version, reacquire current-version chunks
   rather than assembling mixed-version bytes.
5. If no chunk is cached, issue one ordinary full GET. Use its body length as
   current file size and copy into the SDL output. Split the response into owned
   chunks for the bounded session LRU and background IndexedDB persistence.

All cached chunks own complete chunk-sized backing buffers. IndexedDB clones
those same immutable buffers without an additional explicit copy. A small file
that fits in one chunk can reuse its entire response buffer; a multi-chunk file
must not retain its complete backing allocation through cached subarray views.
Streamed reads share these chunks immediately, or join an in-flight full GET.

Concurrent whole-file callers share the network response but each receive
their own SDL allocation. Independent chunk records use independent
transactions; a full-file cache write is not atomic. A complete cached file is
served without revalidation even if it has been removed or changed on the
server. Cache freshness is the application/server's responsibility.

Whole-file loads are intended primarily for small standalone resources.
Applications should keep archives and media on seekable streams, choosing the
whole-file API in their dedicated resource consumers rather than promoting
arbitrary general-file opens.

## Emscripten user storage

Use the legacy JavaScript filesystem and IDBFS. Do not use WasmFS, OPFS,
`autoPersist`, or a custom user-file IndexedDB schema.

Initialization:

1. Obtain the logical preference path from SDL organization/application.
2. Create the mount directory.
3. Mount IDBFS once.
4. Call `FS.syncfs(true)`.
5. Build the startup lowercase-to-actual user map from the populated
   tree.
6. Complete context readiness only after this succeeds.

All user reads/writes/mutations operate on the populated working filesystem.
Streaming writes do not call `syncfs` per transfer.

The following operations call one direct `FS.syncfs(false)` and await it:

- synchronous flush;
- writable sync or async close;
- whole-file save;
- recursive directory creation;
- remove; and
- rename.

There is no dirty flag, generation counter, batching mode, coalescing Promise,
serialization tail, explicit sync API, or retry state. Concurrent operations
may start concurrent `FS.syncfs` calls; redundant work and Emscripten warnings
are accepted. A sync covers the current global IDBFS working tree, so unrelated
open files may be persisted completely or partially. The only promised file
rule is that a successful writable close included that closed file's completed
writes in its sync call.

If `syncfs` fails, the operation reports failure through SDL return/error
conventions or its callback. Close still destroys the handle. No special retry
mechanism exists.

Copy-on-write of an asset fallback loads the asset through the normal whole-
file asset path, creates missing user parents, writes it to the IDBFS working
tree, and opens the new user file. It does not sync the base copy before open;
writable close supplies persistence.

## Emscripten bridges and pthreads

### Async bridge

The JavaScript library starts Fetch, IndexedDB, IDBFS, or proxy work and
returns. Completion creates a C operation record. Browser-thread producers and
the one Pump-thread consumer share one small completion queue protected by one
narrow mutex. This is the only planned SDL_emfs mutex.

The mutex protects only push/swap of completion records. It does not protect
file handles, path indexes, cache maps, transfers, or callbacks.

Browser-main-thread memory hits may complete inline when that can be achieved
without an additional lookup or state representation. Pthread submissions
proxy to the browser-owned JavaScript service and complete later through Pump,
even on a cache hit.

### Blocking bridge

Blocking web functions await the same Promise-based service. SDL_emfs always
compiles these functions into the static library but does not add Asyncify
link options. An application that calls them must enable Asyncify itself.
There are no separate CMake sync/async library targets and no runtime guard.

### Pthread contract

The consuming application opts into Emscripten pthreads and supplies its own
pthread/worker-pool flags. SDL_emfs does not force SharedArrayBuffer or
cross-origin isolation on single-threaded builds.

Both fronts must work in pthread builds. In `PROXY_TO_PTHREAD` applications,
Pump runs on the application's game pthread, not the browser UI thread.
JavaScript Fetch/cache and legacy FS/IDBFS work proxies to the browser thread;
completion crosses the narrow protected queue back to Pump.

Documentation must state the SharedArrayBuffer, worker, COOP, COEP, CORS, and
cross-origin-isolation deployment requirements.

## Build system

- Build one static library.
- Require C++17 for implementation sources.
- Require SDL 3.2.0 or newer.
- Use an existing `SDL3::SDL3` target when present; otherwise use
  `find_package(SDL3 3.2 REQUIRED CONFIG)`.
- Support `add_subdirectory`; do not add install/export package machinery in
  the initial version.
- Do not vendor or FetchContent SDL.
- Do not state an Emscripten minimum version and do not add version-
  compatibility branches.
- Link the normal Emscripten JS library with `--js-library`.
- Link the legacy filesystem and IDBFS support required by the current
  Emscripten toolchain.
- Do not add Asyncify, pthread, LTO, or application deployment flags from the
  library target.
- Use build parallelism `-j24` by default.

The static library contains both blocking and async implementations. Unused
code may be removed by the application's normal linker optimization, but SDL_emfs
does not require LTO and does not attempt to make blocking APIs unavailable in
an async-only build.

## Suggested repository layout

```text
SDL_emfs/
  CMakeLists.txt
  README.md
  include/SDL_emfs/
    SDL_emfs.h
    SDL_emfs_sync.h
    SDL_emfs_async.h
  src/
    context.cpp
    path.cpp
    index.cpp
    sync_file.cpp
    async_file.cpp
    native.cpp
    emscripten/
      web_bridge.cpp
      sdl_emfs_library.js
  tools/
    emscripten_asset_server.py
  examples/
    sync_iostream.cpp
    sync_read_at.cpp
    async_callbacks.cpp
    user_save.cpp
    async_await_adapter.cpp
  docs/
    IMPLEMENTATION_PLAN.md
```

Do not introduce platform backend classes. Compile-time platform translation
units implement a small set of direct internal functions.

## Implementation sequence

### Phase 1: public contract and build

1. Add the static CMake target and SDL discovery.
2. Add the three public headers and fixed C signatures.
3. Add configuration initialization and copied context configuration.
4. Add one-context lifecycle and debug invariant assertions.
5. Add empty native/web translation units and the Emscripten JS-library link.

Exit condition: Linux and Emscripten consumers can include each front and link
the one target; SDL_emfs itself adds no Asyncify or pthread option.

### Phase 2: normalization, indexes, and server

1. Implement shared UTF-8 path normalization and ASCII lowercasing.
2. Implement the immutable C++ asset record and inferred child-list index.
3. Implement synchronous native asset scanning.
4. Implement the startup user case map, rename updates, and live user enumeration.
5. Implement the binary manifest parser.
6. Implement the Python server's lowercase map, binary manifest, Range, ETag,
   CORS, and COOP/COEP behavior.
7. Implement readiness and the Emscripten manifest Fetch.

Exit condition: native and web asset path info/enumeration agree on lowercase
logical files and inferred non-empty directories; user stat/enumeration sees
physical user entries; case-only collisions select an unspecified single
entry.

### Phase 3: native sync and async files

1. Implement root-aware sync open and direct native `OpenIO`.
2. Implement opaque sequential operations and SDL-like status.
3. Implement same-stream save/seek/read/restore `ReadAt`.
4. Implement `LoadFile`, user fallback, user save, and copy-on-write modes.
5. Implement recursive user mkdir, remove, and rename.
6. Implement `SDL_AsyncIOQueue`, async handles, Pump, transfers, and close.
7. Implement native async path operations inline.

Exit condition: one-operation-per-handle behavior works; multiple handles can
run independently; user read fallback/copy-on-write follows the selected mode;
and native IOStream access remains direct.

### Phase 4: web streaming and session cache

1. Add the browser-owned Promise service.
2. Add URL joining and high/low offset conversion.
3. Add the in-flight chunk map and memory LRU.
4. Add exact `206` validation and `200` whole-body fallback.
5. Add multi-chunk reads with browser-controlled concurrency.
6. Add simultaneous automatic prefetch and explicit fire-and-forget prefetch.
7. Add callback completion records, inline memory-hit completion, and Pump.

Exit condition: async-only streaming works without Asyncify, reads chunk
boundaries correctly, shares missing chunks, bounds ready session memory, and
never waits for prefetch.

### Phase 5: persistent asset cache and whole-file load

1. Add one-record-per-chunk IndexedDB storage with identity in the key.
2. Add optional ETag-based identity and per-file invalidation.
3. Add cache corruption recovery and warning-only cache failures.
4. Add background persistence after foreground completion.
5. Copy complete cached coverage directly into the SDL output buffer.
6. Reuse partial coverage; use one full GET only for completely uncached files.
7. Add full-GET deduplication and range joins.
8. Populate the bounded LRU with owned chunks, never large-body subarray views.

Exit condition: warm reloads reuse persistent range or whole-file chunks;
complete whole-file cache reads use no network; incomplete coverage fetches only
missing ranges; wholly uncached files use one full request; and the returned
buffer matches `SDL_LoadFile` ownership.

### Phase 6: IDBFS and blocking web front

1. Mount legacy IDBFS at the SDL preference path.
2. Add initial population to context readiness.
3. Add physical user lookup/enumeration and startup case mapping.
4. Add user open/read/write and asset fallback.
5. Add preserving-mode copy-on-write.
6. Add direct `syncfs(false)` on flush, writable close, save, mkdir, remove,
   and rename.
7. Add blocking web functions over the same Promise service.

Exit condition: a successful ready result exposes populated user files;
writable close and mutations survive reload when their sync succeeds; and the
library target still contributes no Asyncify option.

### Phase 7: pthread handoff

1. Proxy browser-owned JS work from pthread submitters.
2. Add the narrow completion-queue mutex.
3. Deliver Pump callbacks on the application pthread.
4. Validate both async-only pthread and Asyncify+pthreads builds.
5. Add deployment documentation and reference-server headers.

Exit condition: both fronts run in pthread applications, cache state remains
single-owner JS state, and file/index hot paths contain no mutex.

### Phase 8: examples and documentation

1. Add sync IOStream and opaque `ReadAt` examples.
2. Add callback async I/O.
3. Add user save/durability behavior.
4. Add an example-local C++20 async/await adapter.
5. Document initialization, callbacks, ownership, caller sequencing, root
   fallback, copy-on-write, cache identity, server deployment, and deliberate
   limitations.
6. Observe sparse, sequential/prefetched, and small whole-file workloads.

Exit condition: all examples build and run in the applicable validation
configurations and documentation contains no removed API or stronger
durability/cache promise.

## Validation and acceptance

No automated tests are added. Validation uses clean builds, the focused
examples, browser request logs, browser storage inspection, and qualitative
behavior observation.

### Build configurations

- Linux native static library and all applicable examples.
- Emscripten async-only single-threaded.
- Emscripten Asyncify-enabled single-threaded.
- Emscripten async-only pthread.
- Emscripten Asyncify-enabled pthread.

Exact compiler and browser versions are not recorded as a compatibility
contract. SDL 3.2.0 is the only stated dependency floor.

### Functional observations

- Readiness is all-or-nothing for manifest and IDBFS population.
- Asset-cache IndexedDB failure warns but HTTP assets remain usable.
- Asset paths resolve through the lowercase immutable index.
- Native/server case-only collisions choose one result without a public order
  guarantee.
- Empty asset directories remain invisible.
- Physical user mixed-case paths present at startup resolve through the user
  map.
- User-created lowercase files and empty directories are visible immediately.
- Asset reads never inspect user storage.
- User read open/load falls back to assets; user stat/list does not.
- Preserving user writes copy asset content; truncating writes do not.
- Removing a user override reveals an asset fallback again.
- Asset mutations fail.
- Opaque sequential status mirrors SDL behavior.
- `ReadAt` preserves the sequential cursor when the caller obeys the one-
  operation rule.
- Different handles for the same file move and complete independently.
- Async callbacks can submit new work and close an idle handle.
- Whole-file buffers have an uncounted trailing NUL and caller ownership.
- A small streamed read fetches its containing logical chunk.
- Cross-chunk reads copy the correct contiguous bytes.
- Missing chunks in one read start together; automatic prefetch starts at the
  same time but is not awaited.
- Shared missing chunks produce one range request.
- A `200` Range fallback is consumed once and persisted as chunks.
- Complete persistent coverage serves `LoadFile` without HTTP.
- Partial persistent coverage fetches only missing ranges.
- Whole-file disk hits and cold responses populate the bounded memory cache;
  resident repeats do not read IndexedDB.
- Concurrent whole-file loads share one response and return independent SDL
  allocations.
- Foreground reads do not wait for asset-cache writes.
- Corrupt asset-cache records are deleted and refetched.
- Writable user transfers do not call `syncfs` per write.
- Successful writable flush/close, save, and mutations wait for their direct
  `syncfs` call.
- A failed writable close reports failure but releases the handle.
- No final context-destruction sync occurs.
- Blocking Emscripten calls work when the application enables Asyncify.
- Async-only builds execute the async front without Asyncify.
- Both fronts work in single-threaded and pthread applications.

### Qualitative performance observations

- Native `OpenIO` returns a direct SDL file stream.
- Native sequential opaque reads add only file-object access around SDL.
- No file-handle mutex exists.
- Asset indexes are immutable after readiness.
- Web session memory remains bounded by the ready-chunk count plus in-flight
  buffers and caller allocations.
- Whole-file loads do not retain complete response backing through the session
  LRU.
- Web bytes copy directly from JS buffers into the caller's Wasm destination;
  there is no persistent C++ chunk vector.
- Sparse reads, sequential reads with prefetch, and small whole-file loads show
  the expected request shapes in browser logs.

## Deliberate sharp edges

These are accepted constraints, not implementation TODOs:

1. Calling blocking web APIs without application-provided Asyncify is
   undefined.
2. Async submission allocation or queue failure is fatal.
3. One handle may have only one operation in progress.
4. Direct native SDL streams and opaque handles are untracked.
5. Destruction during initialization, pending work, or live handles is caller
   error.
6. There is no cancellation, timeout, automatic retry, or shutdown drain.
7. Fire-and-forget prefetch and cache writes may outlive the C context.
8. Callback timing and thread differ between inline completion and Pump.
9. Asset/user case-fold collisions have an unspecified winner.
10. Native symlinks may escape roots and cyclic asset symlinks are unsupported.
11. Empty asset directories do not exist logically.
12. User read fallback can open a path for which user-root path info reports
    not found, because user stat/list intentionally inspect physical storage.
13. External mixed-case user paths created after readiness may require a game
    restart.
14. Concurrent `syncfs` calls and incidental persistence of other open files
    are accepted.
15. Close failure destroys the handle and has no special retry path.
16. Full cached assets are not revalidated with the server.
17. The cache key omits the full URL and old namespaces are not cleaned.
18. Missing ETags permit undetected same-size content changes.
19. Persistent asset cache writes are background, per-chunk, and non-atomic.
20. Initial native validation covers Linux only; Windows, macOS, and mobile are
    portability targets through SDL but not initial release claims.
