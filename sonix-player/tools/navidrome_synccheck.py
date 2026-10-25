"""Synthetic sync correctness and explicit benchmark; no user credentials/data."""
import argparse
import collections
import gzip
import hashlib
import json
import os
import platform
import shlex
import statistics
import socket
import ssl
import re
import subprocess
import tempfile
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


def run(binary, tracks=16767, albums=1654, delay=0, encoding='identity', close=False,
        mode='normal', environment=None, expected=None, action=None, tls=False):
    counters = collections.Counter()
    salts = set()
    connections = set()
    mutex = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *_):
            pass

        def setup(self):
            super().setup()
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        def handle(self):
            try:
                super().handle()
            except (ConnectionResetError, BrokenPipeError):
                pass

        def do_GET(self):
            url = urlsplit(self.path)
            args = parse_qs(url.query, keep_blank_values=True)
            method = url.path.rsplit('/', 1)[-1]
            if mode == 'redirect' and method == 'search3.view' and '/final/' not in url.path:
                self.send_response(307)
                self.send_header('Location', '/prefix/rest/final/search3.view?' + url.query)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            try:
                assert url.path.startswith('/prefix/rest/')
                salt = args['s'][0]
                assert args['u'] == ['fixture user']
                assert args['t'] == [hashlib.md5(('synthetic-server-password' + salt).encode()).hexdigest()]
                with mutex:
                    assert salt not in salts
                    salts.add(salt)
                    counters[method] += 1
                    connections.add(self.client_address)
                reply = {'status': 'ok', 'version': '1.16.1'}
                if method == 'getScanStatus.view':
                    reply['scanStatus'] = {'scanning': False, 'lastScan': 'fixed-fixture-marker'}
                elif method == 'getGenres.view':
                    reply['genres'] = {'genre': [{'value': 'Jazz'}, {'value': 'Rock'}]}
                    if mode == 'genre-limit':
                        reply['genres']['genre'] += [{'value': f'{i:06d}' + 'x' * 140} for i in range(5000)]
                elif method == 'search3.view':
                    kinds = [('album', 'albumCount', 'albumOffset', albums),
                             ('song', 'songCount', 'songOffset', tracks),
                             ('artist', 'artistCount', 'artistOffset', 20)]
                    selected = [item for item in kinds if int(args[item[1]][0])]
                    assert len(selected) == 1
                    kind, count_key, offset_key, total = selected[0]
                    count, offset = int(args[count_key][0]), int(args[offset_key][0])
                    assert 0 < count <= 1000
                    with mutex:
                        counters[kind] += 1
                        counters['requested'] += count
                    time.sleep(2 if mode == 'stall' else delay)
                    if mode == 'outoforder' and offset % 2000 == 1000:
                        time.sleep(.08)
                    if mode == 'repeat' and offset:
                        offset = 0
                    entries = []
                    for i in range(offset, min(total, offset + count + (1 if mode == 'overcount' else 0))):
                        artist = f'artist-{i % 20:06d}'
                        entry = {'id': f'{kind}-{i:06d}', 'name': f'{kind} {i:06d}',
                                 'artist': 'Fixture artist', 'coverArt': f'cover-{i // 10}',
                                 'artists': [{'id': artist, 'name': 'Fixture artist'}],
                                 'genres': [{'name': 'Jazz'}, {'name': 'Rock'}]}
                        if kind == 'song':
                            entry.update(title=f'Track {i:06d}', albumId=f'album-{i % max(1, albums):06d}',
                                         duration=180, discNumber=1, track=i % 10 + 1)
                        if mode == 'arena':
                            entry['title'] = entry['name'] = 't' * 511
                            entry['artist'] = 'a' * 511
                        if mode == 'rich':
                            entry['ignored'] = 'x' * 20000
                        if mode == 'entitybytes':
                            entry['ignored'] = 'x' * 270000
                        if mode == 'tokens':
                            entry['ignored'] = list(range(17000))
                        entries.append(entry)
                    reply['searchResult3'] = {kind: entries}
                else:
                    assert method == 'ping.view'
                body = json.dumps({'subsonic-response': reply}, separators=(',', ':')).encode()
                if mode == 'depth' and method == 'search3.view':
                    body = body[:-2] + b',"ignored":' + b'[' * 70 + b'0' + b']' * 70 + b'}}'
                if mode == 'dupid' and method == 'search3.view':
                    body = body.replace(b'"id":', b'"id":"ambiguous","id":', 1)
                if mode == 'malformed' and method == 'search3.view':
                    body = body[:-1] + b',}'
                if mode == 'scan-key-nul' and method == 'getScanStatus.view':
                    body = body.replace(b'"scanning"', b'"scanning' + bytes([92]) + b'u0000tail"')
                if mode == 'scan-fields' and method == 'getScanStatus.view':
                    body = body.replace(b'"scanning":false', b'"scanning":false,"scanning":true')
                if mode == 'scan-duplicate' and method == 'getScanStatus.view':
                    body = body[:-2] + b',"status":"ok"}}'
                if mode == 'lateerror' and method == 'search3.view':
                    body = body[:-2] + b',"status":"failed"}}'
                if encoding == 'gzip' and method in ('search3.view', 'getGenres.view'):
                    body = gzip.compress(body)
                    if mode == 'gzipheaders':
                        header = body[:3] + b'\x1e' + body[4:10] + b'\x03\x00abcfixture-name\x00fixture-comment\x00'
                        body = header + (zlib.crc32(header) & 0xffff).to_bytes(2, 'little') + body[10:]
                    if mode == 'concatgzip':
                        body = gzip.compress(b'') + body + gzip.compress(b'')
                    if mode == 'badcrc':
                        body = body[:-8] + bytes([body[-8] ^ 1]) + body[-7:]
                    if mode == 'badgzip':
                        body = body[:-1]
                    self.send_response(200)
                    self.send_header('Content-Encoding', 'br' if mode == 'unsupported' else 'gzip')
                else:
                    self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                if mode in ('chunked', 'bytechunks', 'gzipheaders') and method == 'search3.view':
                    self.send_header('Transfer-Encoding', 'chunked')
                elif mode != 'close-delimited':
                    self.send_header('Content-Length', str(len(body) + (1 if mode == 'truncated' and method == 'search3.view' else 0)))
                if close or mode in ('close-delimited', 'truncated'):
                    self.send_header('Connection', 'close')
                    self.close_connection = True
                self.end_headers()
                with mutex:
                    counters['wire_bytes'] += len(body)
                if mode in ('chunked', 'bytechunks', 'gzipheaders') and method == 'search3.view':
                    step = 1 if mode in ('bytechunks', 'gzipheaders') else 37
                    for at in range(0, len(body), step):
                        part = body[at:at + step]
                        self.wfile.write(f'{len(part):x};fixture=yes\r\n'.encode() + part + b'\r\n')
                    self.wfile.write(b'0\r\nFixture: complete\r\n\r\n')
                elif mode == 'trickle' and method == 'search3.view':
                    for byte in body:
                        self.wfile.write(bytes([byte]))
                        self.wfile.flush()
                        time.sleep(.03)
                else:
                    self.wfile.write(body)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception:
                with mutex:
                    counters['fixture_failures'] += 1
                self.close_connection = True

    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server, tempfile.TemporaryDirectory(prefix='sonix-sync-card-') as card:
        if tls:
            cert, key = os.path.join(card, 'fixture-cert.pem'), os.path.join(card, 'fixture-key.pem')
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                            '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                            '-keyout', key, '-out', cert], check=True, capture_output=True)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            environment = {**(environment or {}), 'SONIX_TEST_CA': cert}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'https://localhost' if tls else 'http://127.0.0.1'
        cmd = [binary, f'{origin}:{server.server_port}/prefix', card, str(tracks), str(albums)]
        if expected:
            cmd.append(expected)
        if action:
            cmd.append(action)
        process = subprocess.run(cmd, capture_output=True, text=True, timeout=100,
                                 env={**os.environ, **(environment or {})})
        server.shutdown()
        forbidden = ('synthetic-server-password', 'fixture user', '?u=', '&t=', 'fixed-fixture-marker')
        assert not any(value in process.stderr for value in forbidden), 'unsafe diagnostics'
        if process.returncode:
            print(process.stderr[-8000:])
        assert process.returncode == 0, f'sync fixture failed ({mode})'
        assert not counters['fixture_failures'], 'fixture request validation failed'
        result = json.loads(process.stdout.strip())
        result.update(counters)
        result['connections'] = len(connections)
        phases = re.search(r'navidrome: baseline timing (.*)', process.stderr)
        if phases:
            result['baseline_phases_ms'] = {key: float(value) for key, value in re.findall(r'(\w+)=([0-9.]+)', phases[1])}
        timing = re.search(r'navidrome: sync timing (.*)', process.stderr)
        if timing:
            result['phases_ms'] = {key: float(value) for key, value in re.findall(r'(\w+)=([0-9.]+)', timing[1])}
        metrics = re.search(r'navidrome: sync metrics (.*)', process.stderr)
        if metrics:
            result['metrics'] = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', metrics[1])}
            assert result['metrics']['buffer_peak'] <= 4 * 1024 * 1024
            assert result['metrics']['slot_peak'] <= 2
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('--benchmark', action='store_true')
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--output')
    parser.add_argument('--baseline-report', default='/tmp/sonix-sync-baseline.json')
    parser.add_argument('--deadline-check', action='store_true')
    args = parser.parse_args()
    if args.deadline_check:
        result = run(args.binary, tracks=10, albums=1, mode='trickle', expected='nd_response_error')
        assert result['seconds'] < 2
        print('whole-response deadline against a trickling server: passed')
        return
    if not args.benchmark and not args.baseline:
        cases = [
            ('normal', {}), ('gzip', {'encoding': 'gzip'}),
            ('tls-gzip', {'tls': True, 'encoding': 'gzip'}),
            ('worker-fallback', {'environment': {'SONIX_SYNC_FAIL_WORKER': '2'}}),
            ('worker-fail', {'environment': {'SONIX_SYNC_FAIL_WORKER': '1'}, 'expected': 'nd_worker_error'}),
            ('concatgzip', {'encoding': 'gzip', 'mode': 'concatgzip'}),
            ('close', {'close': True}),
            ('close-delimited', {'mode': 'close-delimited'}),
            ('truncated', {'mode': 'truncated', 'expected': 'nd_response_error'}),
            ('malformed', {'mode': 'malformed', 'expected': 'nd_response_error'}),
            ('scan-duplicate', {'mode': 'scan-duplicate', 'expected': 'nd_api_error'}),
            ('scan-fields', {'mode': 'scan-fields', 'expected': 'nd_response_error'}),
            ('scan-key-nul', {'mode': 'scan-key-nul', 'expected': 'nd_response_error'}),
            ('genre-limit', {'mode': 'genre-limit', 'expected': 'nd_page_large'}),
            ('redirect', {'tracks': 100, 'albums': 10, 'mode': 'redirect'}),
            ('unsupported', {'mode': 'unsupported', 'encoding': 'gzip', 'expected': 'nd_response_error'}), ('one-slot', {'environment': {'SONIX_SYNC_SLOTS': '1'}}),
            ('exact', {'tracks': 17000, 'albums': 1700}),
            ('outoforder', {'mode': 'outoforder'}),
            ('empty', {'tracks': 0, 'albums': 0}),
            ('large', {'tracks': 100000, 'albums': 10000}),
            ('arena', {'tracks': 1700, 'albums': 170, 'mode': 'arena'}),
            ('rich', {'tracks': 700, 'albums': 70, 'mode': 'rich'}),
            ('decoded-limit', {'tracks': 1000, 'albums': 100, 'mode': 'rich', 'encoding': 'gzip'}),
            ('gzipheaders', {'tracks': 10, 'albums': 1, 'mode': 'gzipheaders', 'encoding': 'gzip'}),
            ('entitybytes', {'tracks': 1, 'albums': 1, 'mode': 'entitybytes', 'expected': 'nd_page_large'}),
            ('overcount', {'mode': 'overcount', 'expected': 'nd_sync_inconsistent'}),
            ('chunked', {'tracks': 70, 'albums': 7, 'mode': 'chunked', 'encoding': 'gzip'}),
            ('bytechunks', {'tracks': 10, 'albums': 1, 'mode': 'bytechunks', 'encoding': 'gzip'}),
            ('tokens', {'tracks': 1, 'albums': 1, 'mode': 'tokens', 'expected': 'nd_page_large'}),
            ('depth', {'mode': 'depth', 'expected': 'nd_json_depth'}),
            ('lateerror', {'mode': 'lateerror', 'expected': 'nd_response_error'}),
            ('dupid', {'mode': 'dupid', 'expected': 'nd_sync_inconsistent'}),
            ('badgzip', {'encoding': 'gzip', 'mode': 'badgzip', 'expected': 'nd_response_error'}),
            ('badcrc', {'encoding': 'gzip', 'mode': 'badcrc', 'expected': 'nd_response_error'}),
            ('repeat', {'mode': 'repeat', 'expected': 'nd_sync_inconsistent'}),
            ('cancel', {'mode': 'stall', 'expected': 'nd_sync_cancelled', 'action': 'cancel'}),
            ('card', {'mode': 'stall', 'expected': 'nd_sync_cancelled', 'action': 'card'}),
            ('forget', {'mode': 'stall', 'expected': 'nd_sync_cancelled', 'action': 'forget'}),
            ('shutdown', {'mode': 'stall', 'expected': 'nd_sync_cancelled', 'action': 'shutdown'}),
            ('lock', {'mode': 'stall', 'expected': 'nd_sync_cancelled', 'action': 'lock'}),
            ('network', {'mode': 'stall', 'expected': 'nd_sync_cancelled', 'action': 'network'}),
            ('reset', {'mode': 'stall', 'expected': 'nd_sync_cancelled', 'action': 'reset'}),
            ('profile', {'mode': 'stall', 'expected': 'nd_sync_cancelled', 'action': 'profile'}),
        ]
        reports = {}
        for name, options in cases:
            result = run(args.binary, **options)
            reports[name] = result
            if name == 'normal':
                assert result['metrics']['track_pages'] == 17
                assert 17 <= result['song'] <= 18
                assert result['connections'] <= 8
                assert result['metrics']['reuses'] > 0
            if name == 'genre-limit':
                assert result['getGenres.view'] == 1
            if name == 'one-slot':
                assert result['song'] == 17
            if name == 'exact':
                assert result['metrics']['track_pages'] == 18
                assert 18 <= result['song'] <= 19
            if name in ('arena', 'rich', 'decoded-limit'):
                assert result['metrics']['reductions'] > 0
            print(f"{name}: passed ({result['seconds']:.3f}s)", flush=True)
        if args.output:
            with open(args.output, 'w', encoding='utf-8') as output:
                json.dump({'cases': reports}, output, indent=2)
        return
    variants = [('default', {})]
    if args.benchmark and not args.baseline:
        variants += [('one-slot', {'SONIX_SYNC_SLOTS': '1'}),
                     ('gzip', {'SONIX_SYNC_GZIP': '1'}),
                     ('close', {'SONIX_SYNC_REUSE': '0'}),
                     ('page-50', {'SONIX_SYNC_PAGE': '50'})]
    reports = {}
    for name, environment in variants:
        results = []
        for _ in range(5 if args.benchmark else 1):
            results.append(run(args.binary, delay=.05 if args.benchmark else 0,
                               encoding='gzip' if name == 'gzip' else 'identity', environment=environment))
        reports[name] = {'runs': results, 'median_seconds': statistics.median(r['seconds'] for r in results)}
    report = {'variant': 'baseline-50' if args.baseline else 'optimized', 'variants': reports}
    report['context'] = {
        'platform': platform.platform(),
        'compiler': subprocess.check_output(
            [*shlex.split(os.environ.get('CC', 'cc')), '--version'], text=True).splitlines()[0],
        'flags': '-std=gnu11 -O1 -g -Wall -Wextra -Werror HOST_BUILD NAVIDROME_TEST',
        'extra_flags': os.environ.get('NAVIDROME_CHECK_FLAGS', ''),
        'tracks': 16767, 'albums': 1654, 'artists': 20, 'genres': 2,
        'request_delay_seconds': .05 if args.benchmark else 0,
        'dedicated_debug': False,
        'tls_allocations': 'not separately instrumented',
        'worker_stack_bytes_each': None if args.baseline else 262144,
        'resolver_stack_bytes_each': None if args.baseline else 262144,
    }
    baseline_path = args.baseline_report
    if args.benchmark and not args.baseline and os.path.exists(baseline_path):
        with open(baseline_path, encoding='utf-8') as source:
            baseline = json.load(source)
        base_median = baseline.get('median_seconds') or baseline['variants']['default']['median_seconds']
        report['baseline_median_seconds'] = base_median
        report['speedup'] = base_median / reports['default']['median_seconds']
        report['target_3x_met'] = report['speedup'] >= 3
    print(json.dumps(report, indent=2))
    if args.output:
        with open(args.output, 'w', encoding='utf-8') as output:
            json.dump(report, output, indent=2)


if __name__ == '__main__':
    main()
