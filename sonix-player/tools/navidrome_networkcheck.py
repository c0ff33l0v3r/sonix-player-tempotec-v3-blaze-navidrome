"""Synthetic loopback fixture; no installed player/server or credentials needed."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import tempfile
from urllib.parse import parse_qs, urlsplit


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # Request URLs contain authentication material; never log them.

    def do_GET(self):
        parsed = urlsplit(self.path)
        query = parse_qs(parsed.query)
        route = parsed.path.split('/')[1]
        if route != 'legacy':
            assert query['u'] == ['fixture user']
            assert query['v'] == ['1.16.1'] and query['c'] == ['SonixPlayer']
            assert query['f'] == ['json']
            expected = hashlib.md5(('synthetic-server-password' + query['s'][0]).encode()).hexdigest()
            assert query['t'] == [expected]
        assert parsed.path.endswith('/rest/ping.view') or route == 'login'
        if route in ('redirect', 'loginredirect'):
            self.send_response(302)
            destination = '/ok/rest/ping.view' if route == 'redirect' else '/login'
            self.send_header('Location', destination + '?' + parsed.query)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        response = {'subsonic-response': {'status': 'ok'}}
        if route == 'auth':
            response = {'subsonic-response': {'status': 'failed', 'error': {'code': 40}}}
        elif route == 'api':
            response = {'subsonic-response': {'status': 'failed', 'error': {'code': 50}}}
        elif route == 'missing':
            response = {'other': 'ok'}
        elif route == 'nulstatus':
            response = {'subsonic-response': {'status': 'ok\x00invalid'}}
        elif route == 'missingstatus':
            response = {'subsonic-response': {'version': '1.16.1'}}
        elif route == 'full':
            response = {'subsonic-response': {'status': 'ok', 'version': '1.16.1', 'type': 'navidrome', 'serverVersion': 'synthetic', 'openSubsonic': True}}
        body = json.dumps(response).encode()
        if route == 'html':
            body = b'<html>not Navidrome</html>'
        elif route == 'login':
            # Deliberate server echoes must never enter diagnostic output.
            body = ('<html>RESPONSE_BODY_CANARY synthetic-server-password ' + parsed.query + '</html>').encode()
        elif route == 'xml':
            body = b'<subsonic-response status="ok" version="1.16.1"/>'
        elif route == 'gzip':
            body = b'\x1f\x8bcompressed-body-canary'
        elif route == 'malformed':
            body = b'{"subsonic-response":'
        elif route in ('oversize', 'chunkoversize', 'nolengthoversize', 'legacy'):
            body += b' ' * 20000
        elif route == 'empty':
            body = b''
        self.send_response(401 if route == 'http401' else 500 if route == 'http500' else 200)
        self.send_header('Content-Type', 'text/html' if route in ('html', 'login') else 'application/xml' if route == 'xml' else 'application/json')
        if route == 'gzip':
            self.send_header('Content-Encoding', 'gzip')
        self.send_header('X-Diagnostic-Canary', 'RESPONSE_HEADER_CANARY')
        chunked = route in ('chunkok', 'chunkcut', 'chunkoversize')
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
        elif route != 'nolengthoversize':
            self.send_header('Content-Length', str(len(body) + (10 if route == 'cutshort' else 0)))
        self.end_headers()
        try:
            if chunked:
                self.wfile.write(f'{len(body):x}\r\n'.encode() + body + b'\r\n')
                if route != 'chunkcut':
                    self.wfile.write(b'0\r\n\r\n')
            else:
                self.wfile.write(body)
        except (ConnectionResetError, BrokenPipeError):
            pass


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with ThreadingHTTPServer(('127.0.0.1', 0), Fixture) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        cases = {
            'ok': 'ok', 'redirect': 'ok', 'auth': 'nd_auth_error', 'api': 'nd_api_error',
            'html': 'nd_web_page', 'malformed': 'nd_response_error',
            'missing': 'nd_response_error', 'oversize': 'nd_response_error',
            'http401': 'nd_response_error',
            'http500': 'nd_api_error', 'empty': 'nd_response_error',
            'cutshort': 'nd_response_error', 'chunkok': 'ok',
            'chunkcut': 'nd_response_error', 'chunkoversize': 'nd_response_error',
            'nolengthoversize': 'nd_response_error',
            'nulstatus': 'nd_response_error',
            'full': 'ok', 'loginredirect': 'nd_web_page',
            'xml': 'nd_response_error', 'gzip': 'nd_response_error',
            'missingstatus': 'nd_response_error',
        }
        diagnostic_cases = {
            'ok': 'reason=subsonic_ok', 'html': 'content_type=html',
            'malformed': 'reason=malformed_json', 'missing': 'reason=missing_subsonic_wrapper',
            'missingstatus': 'reason=missing_or_invalid_status', 'nulstatus': 'reason=status_contains_nul',
            'oversize': 'reason=body_limit', 'chunkcut': 'reason=incomplete_or_invalid_chunks',
            'loginredirect': 'protected redirect status=302 destination=relative',
            'xml': 'content_type=xml', 'gzip': 'encoding=gzip',
        }
        def check_redaction(output):
            for secret in ('?u=', '&t=', 'synthetic-server-password', 'fixture user', 'fixture%20user',
                           'RESPONSE_BODY_CANARY', 'RESPONSE_HEADER_CANARY', 'compressed-body-canary'):
                assert secret not in output, secret
        try:
            for route, expected in cases.items():
                url = f'http://127.0.0.1:{server.server_port}/{route}'
                env = dict(os.environ)
                env.pop('SONIX_LOG', None)
                result = subprocess.run([binary, url, expected], check=True, timeout=20, capture_output=True, text=True, env=env)
                check_redaction(result.stderr)
                if route in diagnostic_cases:
                    assert diagnostic_cases[route] in result.stderr, (route, result.stderr)
            with tempfile.TemporaryDirectory(prefix='sonix-navidrome-log-') as folder:
                log = Path(folder) / 'navidrome.log'
                env['SONIX_LOG'] = str(log)
                subprocess.run([binary, f'http://127.0.0.1:{server.server_port}/loginredirect', 'nd_web_page'],
                               check=True, timeout=20, capture_output=True, env=env)
                output = log.read_text()
                assert 'protected redirect status=302' in output and 'reason=web_page' in output
                assert 'content_type=html' in output and 'navidrome: ping response' in output
                check_redaction(output)
            for plain in (False, True):
                with tempfile.TemporaryDirectory(prefix='sonix-nd-debug-redaction-') as folder:
                    env.pop('SONIX_LOG', None)
                    env['ND_DEBUG_CARD'] = folder
                    if plain:
                        env['ND_TEST_PLAIN'] = '1'
                    else:
                        env.pop('ND_TEST_PLAIN', None)
                    subprocess.run([binary, f'http://127.0.0.1:{server.server_port}/loginredirect', 'nd_web_page'],
                                   check=True, timeout=20, capture_output=True, env=env)
                    files = list((Path(folder) / '.local').glob('navidrome-debug-*.log'))
                    assert len(files) == 1
                    output = files[0].read_text()
                    assert output.count('http: protected response') == 2 and 'reason=web_page' in output
                    check_redaction(output)
                    env.pop('ND_DEBUG_CARD', None)
            subprocess.run([binary, f'http://127.0.0.1:{server.server_port}/legacy/rest/ping.view', 'legacy'], check=True, timeout=20, capture_output=True)
        finally:
            server.shutdown()
            thread.join()
    subprocess.run([binary, f'http://127.0.0.1:{server.server_port}/ok', 'nd_network_error'], check=True, timeout=20, capture_output=True)
    print(f'{len(cases)} real HTTP/authentication/redirect fixtures, refusal, redacted diagnostics/file logging, and legacy prefix behavior passed.')


if __name__ == '__main__':
    main()
