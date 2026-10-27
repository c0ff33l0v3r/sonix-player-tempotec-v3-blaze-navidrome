import http.server
import json
import subprocess
import sys
import threading
import time
import urllib.parse

calls = []
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass
    def do_GET(self):
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        song = query.get('id', [''])[0]
        calls.append(song)
        if self.path.startswith('/search?'):
            assert 'u' not in query and 't' not in query and 's' not in query
            calls[-1] = 'lrclib'
            body = json.dumps([{'trackName': 'Fixture title', 'artistName': 'Fixture artist', 'syncedLyrics': '[00:01.250]words'}]).encode()
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        assert self.path.startswith('/rest/getLyricsBySongId.view?')
        if song.startswith('slow'):
            time.sleep(0.6)
        if song == 'unsupported':
            self.send_error(404)
            return
        if song == 'redirect':
            self.send_response(302)
            self.send_header('Location', '/credential-redirect')
            self.end_headers()
            return
        versions = [] if song == 'empty' else [{'lang': 'eng', 'synced': True, 'line': [{'start': 1234, 'value': 'fixture'}]}]
        lyrics_list = {} if song == 'empty' else {'structuredLyrics': versions}
        body = json.dumps({'subsonic-response': {'status': 'ok', 'lyricsList': lyrics_list}}).encode()
        if song == 'duplicate':
            body = b'{"subsonic-response":{"status":"ok","status":"failed"}}'
        if song == 'tokens':
            body = json.dumps({'subsonic-response': {'status': 'ok', 'noise': [0] * 66000}}).encode()
        if song == 'oversize':
            body = b'x' * (1024 * 1024 + 1)
        self.send_response(200)
        self.send_header('Content-Length', str(len(body) + (100 if song == 'truncated' else 0)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}'], capture_output=True, text=True)
    # Logs must never expose synthetic credentials, query URLs or lyric content.
    for secret in ('fixture-secret', 'fixture-user', '/rest/', 'getLyricsBySongId.view?', '日本語', 'Fixture title', 'Fixture artist'):
        assert secret not in result.stdout + result.stderr
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr, file=sys.stderr)
        raise SystemExit(result.returncode)
    assert calls == ['good', 'empty', 'unsupported', 'duplicate', 'truncated', 'redirect', 'oversize', 'tokens', 'lrclib', 'slow', 'slow-card', 'slow-shutdown', 'normal-quota', 'report-quota', 'lyrics-after-dns'], calls
finally:
    server.shutdown()
    server.server_close()
