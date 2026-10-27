"""Actual detailed images and authenticated loopback source-acquisition checks."""
import hashlib
import io
import subprocess
import sys
import tempfile
import threading
import time
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit
from PIL import Image, ImageDraw

requests = defaultdict(list)
failures = []
recovered = False

def artwork(w, h, fmt='PNG', progressive=False):
    im = Image.new('RGB', (w, h), '#123456')
    draw = ImageDraw.Draw(im)
    for x in range(0, w, 8):
        draw.line((x, 0, x, h), fill='white')
    for y in range(0, h, 8):
        draw.line((0, y, w, y), fill='black')
    draw.text((w // 2 - 30, h // 2), 'SHARP 512', fill='red')
    out = io.BytesIO()
    im.save(out, format=fmt, quality=95, progressive=progressive)
    return out.getvalue()

class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        global recovered
        q = parse_qs(urlsplit(self.path).query)
        if self.path.startswith('/control'):
            recovered = True
            self.reply(b'ok')
            return
        try:
            assert urlsplit(self.path).path == '/rest/getCoverArt.view'
            assert q['u'] == ['art-fixture-user']
            assert q['t'] == [hashlib.md5(('art-fixture-secret' + q['s'][0]).encode()).hexdigest()]
            key, size = q['id'][0], int(q['size'][0])
            assert 1 <= size <= 2048
            requests[key].append(size)
            if key == 'redirect':
                self.send_response(302); self.send_header('Location', '/forbidden'); self.send_header('Content-Length', '0'); self.end_headers(); return
            if key == 'bad': self.reply(b'bad art-fixture-secret'); return
            if key == 'oversize': self.reply(b'x' * (2 * 1024 * 1024 + 1)); return
            if key == 'huge': self.reply(artwork(2049, 2)); return
            if key == 'truncated':
                self.send_response(200); self.send_header('Content-Length', '100000'); self.end_headers(); self.wfile.write(b'short'); self.close_connection = True; return
            if key == 'retry' and size > 512 and not recovered: self.reply(b'failed', 503); return
            w, h = {'portrait': (800, 1200), 'landscape': (1200, 800), 'small': (80, 80), 'panorama': (8000, 100), 'retry': (800, 1200)}.get(key, (1024, 1024))
            if key.startswith('slow-2-'): w, h = 800, 1200
            ratio = min(1, size / max(w, h))
            w, h = max(1, round(w * ratio)), max(1, round(h * ratio))
            if key.startswith('slow-') and (not key.startswith('slow-2-') or size > 512):
                body = artwork(w, h)
                self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers()
                self.wfile.write(body[:8]); self.wfile.flush(); time.sleep(.2)
                try: self.wfile.write(body[8:])
                except (BrokenPipeError, ConnectionResetError): pass
                return
            self.reply(artwork(w, h, 'JPEG' if key in ('jpeg', 'progressive') else 'PNG', key == 'progressive'))
        except Exception:
            failures.append('fixture contract failure')
            self.reply(b'failure', 500)
    def reply(self, body, status=200):
        self.send_response(status); self.send_header('Content-Length', str(len(body))); self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass

def main():
    with tempfile.TemporaryDirectory(prefix='nd-artwork-') as root, ThreadingHTTPServer(('127.0.0.1', 0), Fixture) as server:
        Path(root, 'legacy.png').write_bytes(artwork(320, 320))
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}', root], capture_output=True, text=True, timeout=60)
        server.shutdown()
        assert all(secret not in result.stderr for secret in ('art-fixture-user', 'art-fixture-secret', '&t=', '?u=')), 'credential leak'
        if result.returncode: print(result.stderr[-6000:], file=sys.stderr)
        print(result.stdout, end='')
        assert result.returncode == 0, f'artwork fixture exit {result.returncode}'
        assert requests['portrait'] == [512, 769], requests['portrait']
        assert requests['landscape'] == [512, 769]
        assert requests['panorama'] == [512, 2048]
        assert requests['small'] == [512]
        assert requests['square'] == [512, 512]  # explicit corruption requires reacquisition
        assert requests['retry'] == [512, 769, 769]
        assert requests['bad'] == [512]
        assert requests['no-publish'] == [512]
        assert requests['legacy'] == [512]
        assert 'legacy-sharp' not in requests
        assert requests['stale'] == [512, 512]
        assert 'cancelled' not in requests
        assert requests['slow-audio'] == [512, 512]
        assert requests['slow-player-audio'] == [512, 512]
        assert all(requests[f'slow-1-{n}'] == [512] and requests[f'slow-2-{n}'] == [512, 769] for n in range(1, 7)), requests
        assert not failures, failures
        assert not list(Path(root).glob('art/*.tmp'))
        assert sum(p.stat().st_size for p in Path(root).glob('art/*.img')) <= 32 * 1024 * 1024
        print('authenticated real HTTP request sizes/counts, no redirect replay, byte limits, source budget and redaction: passed')

if __name__ == '__main__': main()
