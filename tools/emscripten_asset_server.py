#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import email.utils
import hashlib
import http.server
import mimetypes
import pathlib
import struct
import urllib.parse


def ascii_lower(value):
    return ''.join(chr(ord(ch) + 32) if 'A' <= ch <= 'Z' else ch for ch in value)


def etag_for(stat):
    value = f'{stat.st_size:x}:{stat.st_mtime_ns:x}'.encode('ascii')
    return '"' + hashlib.sha256(value).hexdigest() + '"'


def build_index(root):
    files = {}
    for path in root.rglob('*'):
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            logical_path = ascii_lower(relative)
            if logical_path != '.sdl-emfs/manifest.bin':
                files.setdefault(logical_path, path)
    return files


def nginx_etag_for(stat):
    return f'"{int(stat.st_mtime):x}-{stat.st_size:x}"'


def build_manifest(files, etag_function=etag_for):
    records = []
    for logical_path, physical_path in files.items():
        stat = physical_path.stat()
        encoded_path = logical_path.encode('utf-8')
        encoded_etag = etag_function(stat).encode('ascii')
        records.append(struct.pack('<QqII', stat.st_size, stat.st_mtime_ns // 1_000_000,
                                   len(encoded_path), len(encoded_etag)) + encoded_path + encoded_etag)
    return b'SDLEMFS1' + struct.pack('<I', len(records)) + b''.join(records)


class AssetServer(http.server.BaseHTTPRequestHandler):
    server_version = 'SDL_emfs/1'

    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Range, If-Range')
        self.send_header('Access-Control-Expose-Headers', 'Accept-Ranges, Content-Length, Content-Range, ETag')
        self.send_header('Cross-Origin-Resource-Policy', 'cross-origin')
        if self.server.isolation:
            self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
            self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(http.HTTPStatus.NO_CONTENT)
        self.end_headers()

    def do_HEAD(self):
        self.serve(send_body=False)

    def do_GET(self):
        self.serve(send_body=True)

    def serve(self, send_body):
        requested = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path).lstrip('/')
        logical_path = ascii_lower(requested.replace('\\', '/'))
        if logical_path == '.sdl-emfs/manifest.bin':
            self.send_bytes(self.server.manifest, 'application/octet-stream', send_body)
            return

        physical_path = self.server.files.get(logical_path)
        if physical_path is None:
            self.send_error(http.HTTPStatus.NOT_FOUND)
            return

        stat = physical_path.stat()
        etag = self.server.etag_function(stat)
        first = 0
        last = stat.st_size - 1
        partial = False
        range_header = self.headers.get('Range')
        if range_header and self.headers.get('If-Range', etag) == etag:
            parsed = self.parse_range(range_header, stat.st_size)
            if parsed is None:
                self.send_response(http.HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                self.send_header('Content-Range', f'bytes */{stat.st_size}')
                self.send_header('ETag', etag)
                self.end_headers()
                return
            first, last = parsed
            partial = True

        length = max(0, last - first + 1)
        self.send_response(http.HTTPStatus.PARTIAL_CONTENT if partial else http.HTTPStatus.OK)
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Type', mimetypes.guess_type(physical_path.name)[0] or 'application/octet-stream')
        self.send_header('Content-Length', str(length))
        self.send_header('ETag', etag)
        self.send_header('Last-Modified', email.utils.formatdate(stat.st_mtime, usegmt=True))
        if partial:
            self.send_header('Content-Range', f'bytes {first}-{last}/{stat.st_size}')
        self.end_headers()

        if send_body and length:
            with physical_path.open('rb') as source:
                source.seek(first)
                remaining = length
                while remaining:
                    block = source.read(min(remaining, 1024 * 1024))
                    if not block:
                        break
                    self.wfile.write(block)
                    remaining -= len(block)

    @staticmethod
    def parse_range(value, size):
        if not value.startswith('bytes=') or ',' in value or size == 0:
            return None
        bounds = value[6:].split('-', 1)
        if len(bounds) != 2:
            return None
        try:
            if not bounds[0]:
                suffix = int(bounds[1])
                if suffix <= 0:
                    return None
                return max(0, size - suffix), size - 1
            first = int(bounds[0])
            last = int(bounds[1]) if bounds[1] else size - 1
        except ValueError:
            return None
        if first < 0 or first >= size or last < first:
            return None
        return first, min(last, size - 1)

    def send_bytes(self, data, content_type, send_body):
        self.send_response(http.HTTPStatus.OK)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        if send_body:
            self.wfile.write(data)


def main():
    parser = argparse.ArgumentParser(description='Serve an SDL_emfs Emscripten asset root')
    parser.add_argument('asset_root', type=pathlib.Path)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8000)
    parser.add_argument('--no-isolation', action='store_true', help='omit COOP and COEP headers')
    parser.add_argument('--nginx-etags', action='store_true', help='match static nginx ETags and prepared manifests')
    arguments = parser.parse_args()

    root = arguments.asset_root.resolve()
    files = build_index(root)
    server = http.server.ThreadingHTTPServer((arguments.bind, arguments.port), AssetServer)
    server.files = files
    server.etag_function = nginx_etag_for if arguments.nginx_etags else etag_for
    server.manifest = build_manifest(files, server.etag_function)
    server.isolation = not arguments.no_isolation
    print(f'Serving {len(files)} SDL_emfs assets from {root} at http://{arguments.bind}:{arguments.port}/')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
