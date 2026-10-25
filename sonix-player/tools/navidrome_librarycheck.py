"""Synthetic paged Subsonic server for the real sync/store/service check."""
import hashlib
import json
import subprocess
import sys
import struct
import zlib
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


class Fixture(BaseHTTPRequestHandler):
    mode = 'normal'
    salts = set()
    failures = []
    reduced = False
    scan_calls = 0
    art_requests = {}

    def log_message(self, *args):
        pass

    def do_GET(self):
        parsed = urlsplit(self.path)
        args = parse_qs(parsed.query, keep_blank_values=True)
        if parsed.path == '/control':
            Fixture.mode = args['mode'][0]
            Fixture.scan_calls = 0
            self.reply({'ok': True})
            return
        try:
            assert parsed.path.startswith('/prefix/rest/')
            salt = args['s'][0]
            assert salt not in Fixture.salts
            Fixture.salts.add(salt)
            assert args['u'] == ['fixture user']
            valid_password = args['t'] == [hashlib.md5(('synthetic-server-password' + salt).encode()).hexdigest()]
            assert valid_password or args['t'] == [hashlib.md5(('replacement-fixture-password' + salt).encode()).hexdigest()]
            assert args['f'] == ['json'] and args['v'] == ['1.16.1']
            method = parsed.path.rsplit('/', 1)[-1]
            assert method in ('ping.view', 'search3.view', 'getGenres.view', 'getScanStatus.view', 'getCoverArt.view')
            if method == 'getCoverArt.view':
                key = args['id'][0]
                Fixture.art_requests[key] = Fixture.art_requests.get(key, 0) + 1
                if key == 'slow':
                    time.sleep(.15)
                if key == 'bad':
                    self.reply_bytes(b'<html>synthetic-server-password</html>')
                    return
                size = 1024 if key == 'big' else 1
                color = b'\x00\x00\xff\xff' if key == 'blue' else b'\xff\x00\x00\xff'
                def chunk(kind, data):
                    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
                png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
                png += chunk(b'IDAT', zlib.compress((b'\0' + color * size) * size)) + chunk(b'IEND', b'')
                self.reply_bytes(png)
                return
            current = Fixture.mode
            response = {'status': 'ok'}
            if current == 'reject' or not valid_password:
                response = {'status': 'failed', 'error': {'code': 40}}
            elif method == 'getScanStatus.view':
                Fixture.scan_calls += 1
                response['scanStatus'] = {'scanning': current == 'scanning', 'lastScan': str(Fixture.scan_calls) if current == 'scanchange' else 'fixed-marker'}
            elif method == 'getGenres.view':
                if current == 'genreslarge':
                    response['ignored'] = [0] * 17000
                response['genres'] = {'genre': [] if current == 'empty' else [{'value': 'Jazz'}, {'value': 'Rock'}]}
            elif method == 'search3.view':
                assert args['query'] == ['']
                kinds = [('album', 'albumCount', 'albumOffset', 1700), ('song', 'songCount', 'songOffset', 17000), ('artist', 'artistCount', 'artistOffset', 20)]
                enabled = [kind for kind in kinds if int(args[kind[1]][0])]
                assert len(enabled) == 1
                kind, count_key, offset_key, total = enabled[0]
                count, offset = int(args[count_key][0]), int(args[offset_key][0])
                assert 0 < count <= 1000
                Fixture.reduced |= count < 50
                if current == 'empty':
                    total = 0
                elif current == 'contributors':
                    total = {'album': 70, 'song': 700, 'artist': 1020}[kind]
                elif current in ('large', 'structured', 'nullable', 'longartist', 'longlabels', 'longlabels_raw', 'tokens'):
                    total = {'album': 70, 'song': 700, 'artist': 20}[kind]
                elif current == 'deleted':
                    total = {'album': 1699, 'song': 16990, 'artist': 20}[kind]
                if current == 'repeat' and offset:
                    offset = 0
                if current == 'slow':
                    time.sleep(.04)
                if current == 'truncated':
                    self.reply_bytes(b'{"subsonic-response":')
                    return
                entries = []
                for i in range(offset, min(total, offset + count)):
                    artist = f'artist-{(i if kind == "album" else i // 10) % 20:02d}'
                    if kind == 'song':
                        entry = {'id': f'song-{i:05d}', 'title': f'Track {17000-i:05d}', 'albumId': 'missing-album' if current == 'badref' and i == 0 else f'album-{i//10:05d}', 'artistId': artist, 'artist': 'Fixture artist', 'discNumber': i % 2 + 1, 'track': i % 5 + 1, 'genres': [{'name': 'Jazz'}]}
                    elif kind == 'album':
                        entry = {'id': f'album-{i:05d}', 'name': f'Album {1700-i:05d}', 'artistId': artist, 'genre': 'Rock'}
                    else:
                        entry = {'id': f'artist-{i:02d}', 'name': f'Artist {i:02d}'}
                        if current == 'contributors':
                            entry = ({'id': f'contributor-{i:04d}', 'name': '########', 'roles': ['performer']}
                                     if i < 1000 else {'id': f'artist-{i-1000:02d}', 'name': f'Artist {i-1000:02d}'})
                    if current == 'structured' and kind != 'artist':
                        # OpenSubsonic identities take precedence over legacy
                        # display/primary fields, which may describe a combined tag.
                        entry['artists'] = [{'id': artist, 'name': 'Fixture artist'}]
                        entry['artistId'] = 'legacy-combined-id'
                        entry['genres'] = [{'name': 'Jazz'}, {'name': 'Rock'}]
                        entry['genre'] = 'Jazz / Rock'
                    elif current == 'nullable' and kind != 'artist':
                        entry.update(artists=None, albumArtists=None, genres=None)
                    elif current == 'badartist' and kind == 'album' and i == 0:
                        entry['artists'] = [{'id': 'missing-structured-artist', 'name': 'Fixture artist'}]
                    elif current == 'badfield' and kind == 'album' and i == 0:
                        entry['name'] = {'unexpected': 'synthetic-server-password'}
                    elif current == 'longartist' and kind == 'song':
                        entry['artist'] = 'Artist; ' * 100
                    elif current in ('longlabels', 'longlabels_raw', 'tokens'):
                        # Exercise both literal and escaped UTF-8 at truncation
                        # boundaries, as well as escaped text that fits decoded.
                        key = 'title' if kind == 'song' else 'name'
                        labels = ['A' * 700, 'B' * 510 + 'é' + 'tail',
                                  'C' * 509 + '界' + 'tail', 'D' * 508 + '🎵' + 'tail',
                                  '界' * 100, 'E' * 511, 'F' * 510 + 'é']
                        entry[key] = labels[i % len(labels)]
                        if kind != 'artist':
                            entry['artist'] = labels[i % len(labels)]
                    elif current == 'longid' and kind == 'song' and i == 0:
                        entry['id'] = 'x' * 256
                    if current in ('tokens', 'tokensingle') and kind == 'album':
                        entry['ignored'] = [0] * (400 if current == 'tokens' else 17000)
                    if current == 'depth' and kind == 'album':
                        nested = 0
                        for _ in range(70):
                            nested = [nested]
                        entry['ignored'] = nested
                    if current in ('large', 'oversingle') and kind == 'album':
                        entry['ignored'] = 'x' * (12000 if current == 'large' else 300000)
                    entries.append(entry)
                response['searchResult3'] = {kind: entries}
            self.reply({'subsonic-response': response})
        except Exception:
            Fixture.failures.append('fixture contract failure')
            self.reply_bytes(b'{}', 500)

    def reply(self, data):
        self.reply_bytes(json.dumps(data, separators=(',', ':'), ensure_ascii=Fixture.mode != 'longlabels_raw').encode())

    def reply_bytes(self, data, status=200):
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    with ThreadingHTTPServer(('127.0.0.1', 0), Fixture) as server, tempfile.TemporaryDirectory(prefix='sonix-nd-library-') as root:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}', root], capture_output=True, text=True, timeout=120)
        server.shutdown()
        forbidden = ['synthetic-server-password', 'vault1', 'fixture user', '?u=', '&t=']
        assert not any(secret in result.stderr for secret in forbidden), 'diagnostics exposed fixture authentication'
        if result.returncode:
            print(result.stderr[-14000:], file=sys.stderr)
        print(result.stdout, end='')
        assert result.returncode == 0, f'library check exited {result.returncode}'
        for reason in ('reason=server_scanning', 'reason=marker_changed', 'reason=missing_track_album',
                       'reason=missing_relation_entity', 'reason=duplicate_entity', 'field=name reason=wrong_type', 'reason=token_limit', 'reason=depth_limit'):
            assert reason in result.stderr, f'missing safe sync diagnostic: {reason}'
        assert not Fixture.failures and Fixture.reduced
        assert Fixture.art_requests.get('red') == 2 and Fixture.art_requests.get('bad') == 1
        assert Fixture.art_requests.get('blue') == 1 and 'uncached-offline' not in Fixture.art_requests
        print('real HTTP fixture authentication, paging, request exclusions and redaction: passed')


if __name__ == '__main__':
    main()
