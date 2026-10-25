"""Protected finite audio framing fixtures; all credentials are synthetic."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import threading
import time
from urllib.parse import parse_qs, urlsplit


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        parsed = urlsplit(self.path)
        q = parse_qs(parsed.query)
        route = parsed.path.split('/')[1]
        if route == 'legacy':
            assert self.headers.get('Icy-MetaData') == '1'
        else:
            assert self.headers.get('Icy-MetaData') is None
            assert q['id'] == ['opaque:/track ?+'] and q['format'] == ['raw']
            assert q['u'] == ['fixture user']
            assert q['t'] == [hashlib.md5(('synthetic-server-password' + q['s'][0]).encode()).hexdigest()]
            assert parsed.path.endswith('/rest/stream.view')
        self.send_response(302 if route == 'redirect' else 200)
        if route == 'redirect':
            self.send_header('Location', '/fixed/rest/stream.view?' + parsed.query)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        body = bytes(i % 251 for i in range(2048))
        self.send_header('Content-Type', 'application/octet-stream')
        if route.startswith('chunk'):
            self.send_header('Transfer-Encoding', 'chunked')
        elif route != 'unknown':
            self.send_header('Content-Length', str(len(body) + (5 if route == 'cut' else 0)))
        if route == 'encoding':
            self.send_header('Content-Encoding', 'gzip')
        self.send_header('X-Secret', q.get('t', ['none'])[0])
        self.end_headers()
        if route == "stall":
            self.wfile.flush()
            time.sleep(2)
            return
        try:
            if route.startswith('chunk'):
                for start in range(0, len(body), 173):
                    part = body[start:start+173]
                    self.wfile.write(f'{len(part):x}\r\n'.encode() + part + b'\r\n')
                if route != 'chunkcut':
                    self.wfile.write(b'0\r\nX-Trailer: fixture\r\n\r\n')
            else:
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with ThreadingHTTPServer(('127.0.0.1', 0), Fixture) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        for route, result in [('fixed','ok'), ('unknown','ok'), ('chunk','ok'),
                              ('cut','cut'), ('chunkcut','cut'), ('redirect','ok'), ('encoding','openfail'), ('stall','stall'), ('legacy','legacy')]:
            completed = subprocess.run([binary, f'http://127.0.0.1:{server.server_port}/{route}', result],
                                       capture_output=True, text=True, timeout=15)
            output = completed.stdout + completed.stderr
            assert 'synthetic-server-password' not in output and 'fixture user' not in output
            assert '?u=' not in output and 'X-Secret' not in output
            if completed.returncode:
                raise AssertionError(f'{route}: {output}')
        server.shutdown()
    print('protected audio stream: prefix/IDs/auth/framing/truncation/redirect/encoding/stalled-cancellation/redaction passed')


if __name__ == '__main__':
    main()
