// SPDX-License-Identifier: GPL-3.0-or-later

addToLibrary({
    $SDL_EMFS: {
        state: null,

        number: function(low, high) {
            return (low >>> 0) + (high >>> 0) * 4294967296;
        },

        error: function(value) {
            var text = value instanceof Error ? value.message : String(value);
            var pointer = _malloc(lengthBytesUTF8(text) + 1);
            stringToUTF8(text, pointer, lengthBytesUTF8(text) + 1);
            return pointer;
        },

        url: function(path) {
            var encoded = path.split('/').map(encodeURIComponent).join('/');
            return SDL_EMFS.state.assetRoot + '/' + encoded;
        },

        syncfs: function(populate) {
            return new Promise(function(resolve, reject) {
                FS.syncfs(populate, function(error) {
                    if (error) {
                        reject(error);
                    } else {
                        resolve();
                    }
                });
            });
        },

        openDatabase: function(namespace) {
            return new Promise(function(resolve, reject) {
                var request = indexedDB.open('SDL_emfs:' + namespace, 1);
                request.onupgradeneeded = function() {
                    var database = request.result;
                    if (!database.objectStoreNames.contains('chunks')) {
                        database.createObjectStore('chunks', { keyPath: 'key' });
                    }
                };
                request.onsuccess = function() { resolve(request.result); };
                request.onerror = function() { reject(request.error); };
            });
        },

        request: function(request) {
            return new Promise(function(resolve, reject) {
                request.onsuccess = function() { resolve(request.result); };
                request.onerror = function() { reject(request.error); };
            });
        },

        key: function(path, size, etag, index) {
            return JSON.stringify([path, SDL_EMFS.state.chunkSize, size, etag, index]);
        },

        meta: function(path, size, etag) {
            var observed = SDL_EMFS.state.observed.get(path);
            return observed || { size: size, etag: etag };
        },

        memoryGet: function(key) {
            var memory = SDL_EMFS.state.memory;
            var value = memory.get(key);
            if (value !== undefined) {
                memory.delete(key);
                memory.set(key, value);
            }
            return value;
        },

        memoryPut: function(key, value) {
            var state = SDL_EMFS.state;
            if (!state.memoryCapacity) {
                return;
            }
            state.memory.delete(key);
            state.memory.set(key, value);
            while (state.memory.size > state.memoryCapacity) {
                state.memory.delete(state.memory.keys().next().value);
            }
        },

        persistentGet: async function(path, size, etag, index) {
            var database = await SDL_EMFS.state.database;
            if (!database) {
                return null;
            }
            var key = SDL_EMFS.key(path, size, etag, index);
            try {
                var transaction = database.transaction('chunks', 'readonly');
                var record = await SDL_EMFS.request(transaction.objectStore('chunks').get(key));
                if (!record) {
                    return null;
                }
                var start = index * SDL_EMFS.state.chunkSize;
                var expected = Math.min(SDL_EMFS.state.chunkSize, size - start);
                if (!(record.data instanceof ArrayBuffer) || record.data.byteLength !== expected) {
                    var removal = database.transaction('chunks', 'readwrite');
                    removal.objectStore('chunks').delete(key);
                    return null;
                }
                return new Uint8Array(record.data);
            } catch (error) {
                console.warn('SDL_emfs IndexedDB read failed:', error);
                return null;
            }
        },

        persistentPut: async function(path, size, etag, index, bytes) {
            var database = await SDL_EMFS.state.database;
            if (!database) {
                return;
            }
            try {
                var chunks = database.transaction('chunks', 'readwrite');
                // Asset chunks own complete, immutable buffers; IndexedDB clones
                // them itself, so another explicit byte copy is unnecessary.
                chunks.objectStore('chunks').put({
                    key: SDL_EMFS.key(path, size, etag, index),
                    data: bytes.buffer
                });
            } catch (error) {
                console.warn('SDL_emfs IndexedDB write failed:', error);
            }
        },

        invalidate: async function(path) {
            var state = SDL_EMFS.state;
            for (var key of Array.from(state.memory.keys())) {
                if (JSON.parse(key)[0] === path) {
                    state.memory.delete(key);
                }
            }
            var database = await state.database;
            if (!database) {
                return;
            }
            try {
                var transaction = database.transaction('chunks', 'readwrite');
                var prefix = '[' + JSON.stringify(path) + ',';
                var range = IDBKeyRange.bound(prefix, prefix + '\uffff');
                var request = transaction.objectStore('chunks').openCursor(range);
                request.onsuccess = function() {
                    var cursor = request.result;
                    if (cursor) {
                        cursor.delete();
                        cursor.continue();
                    }
                };
            } catch (error) {
                console.warn('SDL_emfs IndexedDB invalidation failed:', error);
            }
        },

        observeResponse: function(path, size, etag) {
            var state = SDL_EMFS.state;
            var old = state.observed.get(path);
            if (!old || old.size !== size || old.etag !== etag) {
                if (old) {
                    console.warn('SDL_emfs asset changed while the page was running:', path);
                }
                state.observed.set(path, { size: size, etag: etag });
                SDL_EMFS.invalidate(path);
            }
        },

        releaseFullGet: function(path) {
            // A completed whole-file load can also overlap already-started ranges.
            for (var key of SDL_EMFS.state.inflightChunks.keys()) {
                if (JSON.parse(key)[0] === path) return;
            }
            SDL_EMFS.state.fullGets.delete(path);
        },

        cacheBody: function(path, body, etag) {
            var chunkSize = SDL_EMFS.state.chunkSize;
            for (var index = 0, start = 0; start < body.byteLength; ++index, start += chunkSize) {
                // Each cached chunk owns only its bytes, never an entire archive's
                // backing buffer. Reuse that same buffer for IndexedDB's clone.
                var bytes = body.byteLength <= chunkSize ? body : body.slice(start, Math.min(start + chunkSize, body.byteLength));
                SDL_EMFS.memoryPut(SDL_EMFS.key(path, body.byteLength, etag, index), bytes);
                SDL_EMFS.persistentPut(path, body.byteLength, etag, index, bytes);
            }
        },

        fullGet: function(path, size, etag) {
            var state = SDL_EMFS.state;
            var pending = state.fullGets.get(path);
            if (pending) {
                return pending;
            }
            pending = (async function() {
                var response = await fetch(SDL_EMFS.url(path));
                if (!response.ok) {
                    throw new Error('HTTP ' + response.status + ' while loading ' + path);
                }
                var body = new Uint8Array(await response.arrayBuffer());
                var responseEtag = response.headers.get('ETag') || '';
                if (body.byteLength !== size || responseEtag !== etag) {
                    SDL_EMFS.observeResponse(path, body.byteLength, responseEtag);
                    size = body.byteLength;
                    etag = responseEtag;
                }
                SDL_EMFS.cacheBody(path, body, etag);
                return { body: body, size: body.byteLength, etag: etag };
            })();
            state.fullGets.set(path, pending);
            var finished = function() { SDL_EMFS.releaseFullGet(path); };
            pending.then(finished, finished);
            return pending;
        },

        fetchChunk: async function(path, size, etag, index) {
            var state = SDL_EMFS.state;
            var start = index * state.chunkSize;
            var end = Math.min(start + state.chunkSize, size) - 1;
            var headers = { Range: 'bytes=' + start + '-' + end };
            if (etag && etag.indexOf('W/') !== 0) {
                headers['If-Range'] = etag;
            }
            var response = await fetch(SDL_EMFS.url(path), { headers: headers });
            var bytes = new Uint8Array(await response.arrayBuffer());
            var responseEtag = response.headers.get('ETag') || '';

            if (response.status === 200) {
                // A range already in flight may arrive after another range promoted
                // a full body. Only the first body owns persistence.
                var accepted = state.fullGets.get(path);
                if (accepted) {
                    var existing = await accepted;
                    return SDL_EMFS.memoryGet(SDL_EMFS.key(path, existing.size, existing.etag, index))
                        || existing.body.slice(start, Math.min(start + state.chunkSize, existing.size));
                }
                if (bytes.byteLength !== size || responseEtag !== etag) {
                    SDL_EMFS.observeResponse(path, bytes.byteLength, responseEtag);
                    size = bytes.byteLength;
                    etag = responseEtag;
                }
                var result = { body: bytes, size: bytes.byteLength, etag: etag };
                var promoted = Promise.resolve(result);
                state.fullGets.set(path, promoted);
                SDL_EMFS.cacheBody(path, bytes, etag);
                return SDL_EMFS.memoryGet(SDL_EMFS.key(path, bytes.byteLength, etag, index))
                    || bytes.slice(start, Math.min(start + state.chunkSize, bytes.byteLength));
            }
            if (response.status !== 206) {
                throw new Error('HTTP ' + response.status + ' while reading ' + path);
            }
            var contentRange = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(response.headers.get('Content-Range') || '');
            if (!contentRange || Number(contentRange[1]) !== start || Number(contentRange[2]) !== end || bytes.byteLength !== end - start + 1) {
                throw new Error('Invalid Content-Range while reading ' + path);
            }
            var total = Number(contentRange[3]);
            if (total !== size || responseEtag !== etag) {
                SDL_EMFS.observeResponse(path, total, responseEtag);
                size = total;
                etag = responseEtag;
            }
            SDL_EMFS.persistentPut(path, size, etag, index, bytes);
            return bytes;
        },

        ensureChunk: function(path, suppliedSize, suppliedEtag, index, cacheMiss) {
            var state = SDL_EMFS.state;
            var meta = SDL_EMFS.meta(path, suppliedSize, suppliedEtag);
            var key = SDL_EMFS.key(path, meta.size, meta.etag, index);
            var ready = SDL_EMFS.memoryGet(key);
            if (ready !== undefined) {
                return Promise.resolve(ready);
            }
            var pending = state.inflightChunks.get(key);
            if (pending) {
                return pending;
            }
            var full = state.fullGets.get(path);
            if (full) {
                return full.then(function(result) {
                    var start = index * state.chunkSize;
                    var key = SDL_EMFS.key(path, result.size, result.etag, index);
                    var bytes = SDL_EMFS.memoryGet(key);
                    if (bytes === undefined) {
                        bytes = result.body.slice(start, Math.min(start + state.chunkSize, result.size));
                        SDL_EMFS.memoryPut(key, bytes);
                    }
                    return bytes;
                });
            }
            var inflightKey = key;
            pending = (async function() {
                var bytes = cacheMiss ? null : await SDL_EMFS.persistentGet(path, meta.size, meta.etag, index);
                if (!bytes) {
                    bytes = await SDL_EMFS.fetchChunk(path, meta.size, meta.etag, index);
                    var current = SDL_EMFS.meta(path, meta.size, meta.etag);
                    meta = current;
                    key = SDL_EMFS.key(path, meta.size, meta.etag, index);
                }
                SDL_EMFS.memoryPut(key, bytes);
                return bytes;
            })();
            state.inflightChunks.set(inflightKey, pending);
            var finished = function() {
                state.inflightChunks.delete(inflightKey);
                // Release promoted bodies after all already-started ranges finish,
                // not after the much faster IndexedDB transaction submission.
                SDL_EMFS.releaseFullGet(path);
            };
            pending.then(finished, finished);
            return pending;
        },

        readCached: function(path, size, etag, offset, destination, length) {
            var state = SDL_EMFS.state;
            var meta = SDL_EMFS.meta(path, size, etag);
            if (offset >= meta.size || length === 0) {
                return 0;
            }
            length = Math.min(length, meta.size - offset);
            var first = Math.floor(offset / state.chunkSize);
            var last = Math.floor((offset + length - 1) / state.chunkSize);
            var chunks = [];
            for (var index = first; index <= last; ++index) {
                var key = SDL_EMFS.key(path, meta.size, meta.etag, index);
                var chunk = state.memory.get(key);
                if (chunk === undefined) {
                    return -1;
                }
                chunks.push({ key: key, bytes: chunk });
            }
            var copied = 0;
            for (var chunkIndex = first; chunkIndex <= last; ++chunkIndex) {
                var cached = chunks[chunkIndex - first];
                state.memory.delete(cached.key);
                state.memory.set(cached.key, cached.bytes);
                var chunkStart = chunkIndex * state.chunkSize;
                var from = Math.max(offset, chunkStart) - chunkStart;
                var to = Math.min(offset + length, chunkStart + cached.bytes.byteLength) - chunkStart;
                if (to > from) {
                    HEAPU8.set(cached.bytes.subarray(from, to), destination + copied);
                    copied += to - from;
                }
            }
            SDL_EMFS.readAhead(path, meta, last);
            return copied;
        },

        readAhead: function(path, meta, last) {
            for (var ahead = 1; ahead <= SDL_EMFS.state.prefetch; ++ahead) {
                var next = last + ahead;
                if (next * SDL_EMFS.state.chunkSize < meta.size) {
                    var key = SDL_EMFS.key(path, meta.size, meta.etag, next);
                    if (SDL_EMFS.state.memory.has(key) || SDL_EMFS.state.inflightChunks.has(key)) {
                        continue;
                    }
                    SDL_EMFS.ensureChunk(path, meta.size, meta.etag, next).catch(function() {});
                }
            }
        },

        read: async function(path, size, etag, offset, destination, length) {
            var meta = SDL_EMFS.meta(path, size, etag);
            if (offset >= meta.size || length === 0) {
                return 0;
            }
            length = Math.min(length, meta.size - offset);
            var first = Math.floor(offset / SDL_EMFS.state.chunkSize);
            var last = Math.floor((offset + length - 1) / SDL_EMFS.state.chunkSize);
            var promises = [];
            for (var index = first; index <= last; ++index) {
                promises.push(SDL_EMFS.ensureChunk(path, meta.size, meta.etag, index));
            }
            SDL_EMFS.readAhead(path, meta, last);
            var chunks = await Promise.all(promises);
            var copied = 0;
            for (var chunkIndex = first; chunkIndex <= last; ++chunkIndex) {
                var chunk = chunks[chunkIndex - first];
                var chunkStart = chunkIndex * SDL_EMFS.state.chunkSize;
                var from = Math.max(offset, chunkStart) - chunkStart;
                var to = Math.min(offset + length, chunkStart + chunk.byteLength) - chunkStart;
                if (to > from) {
                    HEAPU8.set(chunk.subarray(from, to), destination + copied);
                    copied += to - from;
                }
            }
            return copied;
        },

        prefetch: function(path, size, etag, offset, length) {
            var meta = SDL_EMFS.meta(path, size, etag);
            if (!length || offset >= meta.size) {
                return;
            }
            var end = Math.min(offset + length, meta.size);
            var first = Math.floor(offset / SDL_EMFS.state.chunkSize);
            var last = Math.floor((end - 1) / SDL_EMFS.state.chunkSize);
            for (var index = first; index <= last; ++index) {
                SDL_EMFS.ensureChunk(path, meta.size, meta.etag, index).catch(function() {});
            }
        },

        probeChunk: async function(path, size, etag, index) {
            var key = SDL_EMFS.key(path, size, etag, index);
            var bytes = SDL_EMFS.memoryGet(key);
            if (bytes !== undefined) {
                return bytes;
            }
            var pending = SDL_EMFS.state.inflightChunks.get(key);
            if (pending) {
                return pending;
            }
            bytes = await SDL_EMFS.persistentGet(path, size, etag, index);
            if (bytes) {
                SDL_EMFS.memoryPut(key, bytes);
            }
            return bytes;
        },

        load: async function(path, size, etag) {
            var meta = SDL_EMFS.meta(path, size, etag);
            var count = Math.ceil(meta.size / SDL_EMFS.state.chunkSize);
            var chunks = await Promise.all(Array.from({ length: count }, function(_, index) {
                return SDL_EMFS.probeChunk(path, meta.size, meta.etag, index);
            }));
            if (count && chunks.every(function(chunk) { return chunk === null; })) {
                // One request for a wholly uncached file; partial cache hits below
                // keep their bytes and request only the missing ranges.
                var result = await SDL_EMFS.fullGet(path, meta.size, meta.etag);
                chunks = [result.body];
                meta = result;
            } else {
                chunks = await Promise.all(chunks.map(function(chunk, index) {
                    return chunk || SDL_EMFS.ensureChunk(path, meta.size, meta.etag, index, true);
                }));
                var current = SDL_EMFS.meta(path, meta.size, meta.etag);
                if (current.size !== meta.size || current.etag !== meta.etag) {
                    // Do not assemble cached bytes from two asset versions.
                    return SDL_EMFS.load(path, current.size, current.etag);
                }
            }
            var buffer = _malloc(meta.size + 1);
            if (!buffer) {
                throw new Error('Out of memory loading ' + path);
            }
            var position = buffer;
            for (var chunk of chunks) {
                HEAPU8.set(chunk, position);
                position += chunk.byteLength;
            }
            HEAPU8[buffer + meta.size] = 0;
            return { buffer: buffer, size: meta.size };
        }
    },

    SDL_EMFS_JS_Init__deps: ['$SDL_EMFS'],
    SDL_EMFS_JS_Init__proxy: 'async',
    SDL_EMFS_JS_Init: function(context, assetRoot, userRoot, applicationNamespace, chunkSize, prefetchChunks, memoryCacheChunks) {
        assetRoot = UTF8ToString(assetRoot).replace(/\/$/, '');
        userRoot = UTF8ToString(userRoot);
        applicationNamespace = UTF8ToString(applicationNamespace);
        var state = {
            assetRoot: assetRoot,
            chunkSize: chunkSize >>> 0,
            prefetch: prefetchChunks >>> 0,
            memoryCapacity: memoryCacheChunks >>> 0,
            memory: new Map(),
            inflightChunks: new Map(),
            fullGets: new Map(),
            observed: new Map(),
            database: SDL_EMFS.openDatabase(applicationNamespace).catch(function(error) {
                console.warn('SDL_emfs IndexedDB cache unavailable:', error);
                return null;
            }),
            ready: null
        };
        SDL_EMFS.state = state;
        state.ready = (async function() {
            try {
                FS.mkdirTree(userRoot);
                FS.mount(IDBFS, {}, userRoot);
                var results = await Promise.all([
                    fetch(assetRoot + '/.sdl-emfs/manifest.bin').then(function(response) {
                        if (!response.ok) {
                            throw new Error('HTTP ' + response.status + ' while loading SDL_emfs manifest');
                        }
                        return response.arrayBuffer();
                    }),
                    SDL_EMFS.syncfs(true)
                ]);
                var manifest = new Uint8Array(results[0]);
                var pointer = _malloc(manifest.byteLength);
                if (!pointer && manifest.byteLength) {
                    throw new Error('Out of memory loading SDL_emfs manifest');
                }
                HEAPU8.set(manifest, pointer);
                state.initializationReported = true;
                if (!_SDL_EMFS_WebInitializationComplete(context, 1, pointer, manifest.byteLength, 0)) {
                    throw new Error('SDL_emfs rejected the asset manifest');
                }
            } catch (error) {
                if (!state.initializationReported) {
                    state.initializationReported = true;
                    _SDL_EMFS_WebInitializationComplete(context, 0, 0, 0, SDL_EMFS.error(error));
                }
                throw error;
            }
        })();
        state.ready.catch(function() {});
    },

    SDL_EMFS_JS_WaitReady__deps: ['$SDL_EMFS'],
    SDL_EMFS_JS_WaitReady__async: 'auto',
    SDL_EMFS_JS_WaitReady__proxy: 'sync',
    SDL_EMFS_JS_WaitReady: function() {
        if (!SDL_EMFS.state) {
            return Promise.resolve(0);
        }
        return SDL_EMFS.state.ready.then(function() { return 1; }, function() { return 0; });
    },

    SDL_EMFS_JS_Read__deps: ['$SDL_EMFS', '$Asyncify'],
    SDL_EMFS_JS_Read__async: true,
    SDL_EMFS_JS_Read__proxy: 'sync',
    SDL_EMFS_JS_Read: function(file, path, etag, sizeLow, sizeHigh, offsetLow, offsetHigh, destination, length) {
        return Asyncify.handleSleep(function(wakeUp) {
            path = UTF8ToString(path);
            etag = UTF8ToString(etag);
            var size = SDL_EMFS.number(sizeLow, sizeHigh);
            var offset = SDL_EMFS.number(offsetLow, offsetHigh);
            var complete = function(bytes) {
                var meta = SDL_EMFS.meta(path, size, etag);
                if (meta.size !== size || meta.etag !== etag) {
                    _SDL_EMFS_WebAssetChanged(file, meta.size >>> 0, Math.floor(meta.size / 4294967296) >>> 0, SDL_EMFS.error(meta.etag));
                }
                wakeUp(bytes);
            };
            var cached = SDL_EMFS.readCached(path, size, etag, offset, destination, length >>> 0);
            if (cached >= 0) {
                complete(cached);
                return;
            }
            SDL_EMFS.read(path, size, etag, offset, destination, length >>> 0).then(complete,
                function(error) { _SDL_EMFS_WebSetError(SDL_EMFS.error(error)); wakeUp(-1); }
            );
        });
    },

    SDL_EMFS_JS_ReadAsync__deps: ['$SDL_EMFS'],
    SDL_EMFS_JS_ReadAsync__proxy: 'async',
    SDL_EMFS_JS_ReadAsync: function(operation, path, etag, sizeLow, sizeHigh, offsetLow, offsetHigh, destination, lengthLow, lengthHigh) {
        path = UTF8ToString(path);
        etag = UTF8ToString(etag);
        var size = SDL_EMFS.number(sizeLow, sizeHigh);
        SDL_EMFS.read(path, size, etag, SDL_EMFS.number(offsetLow, offsetHigh), destination, SDL_EMFS.number(lengthLow, lengthHigh)).then(
            function(bytes) {
                var meta = SDL_EMFS.meta(path, size, etag);
                if (meta.size !== size || meta.etag !== etag) {
                    _SDL_EMFS_WebAsyncAssetChanged(operation, meta.size >>> 0, Math.floor(meta.size / 4294967296) >>> 0, SDL_EMFS.error(meta.etag));
                }
                _SDL_EMFS_WebTransferComplete(operation, 1, bytes >>> 0, Math.floor(bytes / 4294967296) >>> 0, 0);
            },
            function(error) { _SDL_EMFS_WebTransferComplete(operation, 0, 0, 0, SDL_EMFS.error(error)); }
        );
    },

    SDL_EMFS_JS_Prefetch__deps: ['$SDL_EMFS'],
    SDL_EMFS_JS_Prefetch__proxy: 'async',
    SDL_EMFS_JS_Prefetch: function(path, etag, sizeLow, sizeHigh, offsetLow, offsetHigh, lengthLow, lengthHigh) {
        SDL_EMFS.prefetch(UTF8ToString(path), SDL_EMFS.number(sizeLow, sizeHigh), UTF8ToString(etag), SDL_EMFS.number(offsetLow, offsetHigh), SDL_EMFS.number(lengthLow, lengthHigh));
    },

    SDL_EMFS_JS_Load__deps: ['$SDL_EMFS'],
    SDL_EMFS_JS_Load__async: 'auto',
    SDL_EMFS_JS_Load__proxy: 'sync',
    SDL_EMFS_JS_Load: function(path, etag, sizeLow, sizeHigh, bufferOut, sizeOut) {
        path = UTF8ToString(path);
        etag = UTF8ToString(etag);
        return SDL_EMFS.load(path, SDL_EMFS.number(sizeLow, sizeHigh), etag).then(
            function(result) { setValue(bufferOut, result.buffer, '*'); setValue(sizeOut, result.size, 'i32'); return 1; },
            function(error) { _SDL_EMFS_WebSetError(SDL_EMFS.error(error)); return 0; }
        );
    },

    SDL_EMFS_JS_LoadAsync__deps: ['$SDL_EMFS'],
    SDL_EMFS_JS_LoadAsync__proxy: 'async',
    SDL_EMFS_JS_LoadAsync: function(operation, path, etag, sizeLow, sizeHigh) {
        path = UTF8ToString(path);
        etag = UTF8ToString(etag);
        SDL_EMFS.load(path, SDL_EMFS.number(sizeLow, sizeHigh), etag).then(
            function(result) { _SDL_EMFS_WebLoadComplete(operation, result.buffer, result.size, 0); },
            function(error) { _SDL_EMFS_WebLoadComplete(operation, 0, 0, SDL_EMFS.error(error)); }
        );
    },

    SDL_EMFS_JS_Sync__deps: ['$SDL_EMFS'],
    SDL_EMFS_JS_Sync__async: 'auto',
    SDL_EMFS_JS_Sync__proxy: 'sync',
    SDL_EMFS_JS_Sync: function() {
        return SDL_EMFS.syncfs(false).then(function() { return 1; }, function(error) {
            _SDL_EMFS_WebSetError(SDL_EMFS.error(error));
            return 0;
        });
    },

    SDL_EMFS_JS_SyncAsync__deps: ['$SDL_EMFS'],
    SDL_EMFS_JS_SyncAsync__proxy: 'async',
    SDL_EMFS_JS_SyncAsync: function(operation) {
        SDL_EMFS.syncfs(false).then(
            function() { _SDL_EMFS_WebSyncComplete(operation, 1, 0); },
            function(error) { _SDL_EMFS_WebSyncComplete(operation, 0, SDL_EMFS.error(error)); }
        );
    }
});
