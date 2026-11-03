#!/usr/bin/env python3
"""Barrier-driven real-worker artwork regressions; --baseline expects defects."""
import argparse
import hashlib
import os
from pathlib import Path
import select
import shlex
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
from navidrome_artworkcheck import artwork

class State:
    def __init__(self):
        self.started = threading.Event()
        self.closed = threading.Event()
        self.release = threading.Event()
        self.order = []
        self.lock = threading.Lock()
        self.counts = {}
        self.errors = []

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def reply(self, data=b'ok', status=200):
        self.send_response(status); self.send_header('Content-Length', str(len(data))); self.end_headers()
        try: self.wfile.write(data); self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError): pass
    def do_GET(self):
        s = self.server.state
        q = parse_qs(urlsplit(self.path).query)
        if self.path.startswith('/control'):
            if 'wait' in q: ok = s.started.wait(6)
            elif 'closed' in q: ok = s.closed.wait(6)
            elif 'release' in q: s.release.set(); ok = True
            elif 'reset' in q: s.started.clear(); s.closed.clear(); ok = True
            else:
                with s.lock: ok = bool(s.order) and s.order[0] == q['assert_first'][0]
            self.reply(status=200 if ok else 500)
            return
        if (q.get('u') != ['art-fixture-user'] or 's' not in q or
                q.get('t') != [hashlib.md5(('art-fixture-secret' + q['s'][0]).encode()).hexdigest()]):
            self.reply(b'rejected', 401); return
        key = q['id'][0]
        with s.lock:
            if key != 'cached': s.order.append(key)
            s.counts[key] = s.counts.get(key, 0) + 1
            attempt = s.counts[key]
        body = self.server.image
        if key == 'bad': self.reply(b'invalid'); return
        if key == 'portrait':
            size = int(q['size'][0]); body = artwork(size, round(size * 2 / 3))
        held = key == 'held' or (key == 'interrupted' and attempt == 1) or (key == 'repeated' and attempt <= 3)
        if not held:
            self.reply(body); return
        self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers()
        try: self.wfile.write(body[:8]); self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError): return
        s.started.set()
        while not s.release.is_set():
            readable, _, _ = select.select([self.connection], [], [], .05)
            if readable:
                try:
                    if not self.connection.recv(1): s.closed.set(); break
                except ConnectionResetError: s.closed.set(); break
        try: self.wfile.write(body[8:]); self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError): pass

def main():
    a = argparse.ArgumentParser(); a.add_argument('--baseline', action='store_true'); args = a.parse_args()
    player = Path(__file__).resolve().parents[1]; os.chdir(player)
    objects = Path(os.environ.get('SONIX_LVGL_OBJECTS', 'build_host/lvgl')).resolve()
    if not (objects / 'src/lv_init.o').exists():
        raise SystemExit('Requires existing host LVGL objects; set SONIX_LVGL_OBJECTS. No full build is run.')
    with tempfile.TemporaryDirectory(prefix='sonix-artscheduler-') as tmp:
        t = Path(tmp); extra = []
        stream = 'src/system/streaming/streamturn.c'
        if args.baseline:
            for name in ['navidromeart.c', 'navidromeart.h']:
                data = subprocess.check_output(['git', 'show', 'ef4a4e6:sonix-player/src/gui/streaming/' + name])
                (t/name).write_bytes(data)
            (t/'streamturn.c').write_bytes(subprocess.check_output(['git', 'show', 'ef4a4e6:sonix-player/src/system/streaming/streamturn.c']))
            (t/'streamturn.h').write_bytes(subprocess.check_output(['git', 'show', 'ef4a4e6:sonix-player/src/system/streaming/streamturn.h']))
            stream = str(t/'streamturn.c')
            extra = ['-DNAVIDROME_ART_BASELINE', '-DNAVIDROME_ART_IMPLEMENTATION="' + str(t/'navidromeart.c') + '"']
        sources = ['lvgl/src/font/lv_font_unscii_8.c', 'tools/navidrome_artschedulercheck.c', 'src/gui/streaming/navidromethumb.c', 'src/gui/shell/icons.c',
                   'src/gui/nowplaying/cover.c', 'src/system/image/stb_image_impl.c', 'src/system/image/jpeg_scaled.c',
                   'src/system/image/png_scaled.c', 'src/system/image/art_shrink.c', 'src/gb/miniz/miniz.c',
                   'src/system/streaming/navidromeapi.c', 'src/system/streaming/navidromecrypto.c',
                   'src/system/core/md5.c', 'src/system/device/workactivity.c', stream,
                   'src/system/net/http.c', 'src/system/net/tls.c', 'src/system/net/gzip.c']
        command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-missing-field-initializers', '-ffunction-sections', '-fdata-sections',
                   '-DHOST_BUILD', '-DLV_FONT_UNSCII_8=1', '-DLV_CONF_INCLUDE_SIMPLE', '-I.', '-Ilvgl']
        command += shlex.split(os.environ.get('NAVIDROME_CHECK_FLAGS', '')) + extra + sources
        command += [str(p) for p in objects.rglob('*.o')]
        command += ['-Wl,--gc-sections,--wrap=cover_load_image_memory,--wrap=malloc,--wrap=write,--wrap=getaddrinfo,--wrap=pthread_create,--wrap=nd_api_artwork_source,--wrap=poll',
                    '-lm', '-pthread', '-ldl', '-lresolv']
        command += shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'freetype2', 'sdl2'], text=True))
        command += ['-o', str(t/'check')]
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode: raise SystemExit(build.stderr[-6000:])
        modes = ['cached', 'priority', 'recovery']
        if not args.baseline: modes += ['preempt', 'coalesce', 'cached-pressure', 'background', 'stale', 'offline', 'quality', 'failure', 'repeated', 'fair', 'player-ui', 'thread-fail-net', 'thread-fail-local', 'clear', 'observers', 'resources', 'publication', 'allocation', 'retire-lock', 'retire-profile', 'retire-card', 'clear-decode', 'withheld', 'stale-decode', 'clear-handoff', 'shutdown']
        for mode in modes:
            for width, height in ([(320, 480)] if args.baseline else [(320, 480), (480, 800), (480, 720)]):
                state = State()
                with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server, tempfile.TemporaryDirectory(dir=tmp) as card:
                    server.daemon_threads = True; server.state = state; server.image = artwork(512, 512)
                    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
                    try:
                        result = subprocess.run([str(t/'check'), f'http://127.0.0.1:{server.server_port}', card, mode, str(width), str(height)],
                                                capture_output=True, text=True, timeout=18)
                    finally: state.release.set(); server.shutdown(); thread.join()
                    if args.baseline:
                        assert result.returncode != 0, f'{mode}: expected baseline defect was not reproduced'
                        expected = {'cached': 'cached player must finish', 'priority': 'scheduler_control', 'recovery': 'preempted visible thumbnail must recover'}[mode]
                        assert expected in result.stderr, f'{mode}: baseline failed for an unrelated reason'
                        print(f'{mode}: baseline defect reproduced (exit {result.returncode})')
                    else:
                        if result.returncode: print(result.stderr[-5000:])
                        assert result.returncode == 0, f'{mode} width {width}: failed'
                        if mode == 'cached': assert state.counts['cached'] == 1
                        if mode == 'recovery': assert state.counts['interrupted'] == 2
                        if mode == 'coalesce': assert state.counts['held'] == 1
                        if mode == 'publication': assert state.counts['unpublished'] == 1
                        if mode == 'offline': assert list(state.counts) == ['cached']
                        if mode == 'failure': assert state.counts['bad'] == 1
                        if mode == 'quality': assert state.counts['portrait'] == 2
                        if mode == 'repeated': assert state.counts['repeated'] == 4
                        assert not any(secret in result.stderr for secret in ['art-fixture-secret', 'art-fixture-user', '&t='])
                        print(f'{mode} {width}x{height}: passed; request counts {sorted(state.counts.values())}', flush=True)
                        if mode == 'resources': print(result.stdout, end='', flush=True)
if __name__ == '__main__': main()
