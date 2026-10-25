"""Real Subsonic fixtures for independent favorite album/song caches."""
import gzip
import re
import hashlib
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

sys.dont_write_bytecode = True
import navidrome_librarycheck as library


class Fixture(library.Fixture):
    protocol_version = 'HTTP/1.1'
    active_lock = threading.Lock()
    active = peak_active = 0
    compressed = 0
    large_pages = []
    large_run = 0
    first_page_started = threading.Event()
    second_page_started = threading.Event()
    out_of_order = False
    mode = 'normal'
    calls = 0
    scans = 0
    byte_reduced = False
    token_reduced = False
    exact_empty = False
    stars = {}
    mutation_counts = {}
    mutations = 0
    mode_mutations = 0
    lost_mutations = 0

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def reply_bytes(self, data, status=200):
        compress = 'gzip' in self.headers.get('Accept-Encoding', '')
        if compress:
            data = gzip.compress(data, mtime=0)
            Fixture.compressed += 1
            if Fixture.mode == 'gzip_bad':
                data = data[:-4] + b'bad!'
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        if compress:
            self.send_header('Content-Encoding', 'gzip')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def do_GET(self):
        parsed = urlsplit(self.path)
        args = parse_qs(parsed.query, keep_blank_values=True)
        if parsed.path == '/control':
            Fixture.mode = args['mode'][0]
            if Fixture.mode == 'large':
                Fixture.large_run += 1
            Fixture.mode_mutations = 0
            Fixture.scans = 0
            library.Fixture.mode = 'empty' if Fixture.mode == 'library_empty' else 'normal'
            self.reply({'ok': True})
            return
        if Fixture.mode == 'library_empty':
            return super().do_GET()
        try:
            assert parsed.path.startswith('/prefix/rest/')
            assert args['u'][0] in ('fixture user', 'different fixture user')
            salt = args['s'][0]
            assert salt not in library.Fixture.salts
            library.Fixture.salts.add(salt)
            assert args['t'] == [hashlib.md5(('synthetic-server-password' + salt).encode()).hexdigest()]
            assert args['f'] == ['json'] and args['v'] == ['1.16.1']
            method = parsed.path.rsplit('/', 1)[-1]
            assert method in ('star.view', 'unstar.view', 'getAlbum.view', 'getSong.view', 'getArtist.view', 'ping.view', 'getAlbumList2.view', 'search3.view', 'getScanStatus.view')
            current = Fixture.mode
            if current in ('local_edits', 'cache_retry', 'album_pending', 'album_details_fail', 'album_details_ok', 'album_duplicate', 'album_oversize', 'artist_persisted'):
                assert method not in ('search3.view', 'getAlbumList2.view', 'getScanStatus.view'), 'incremental action enumerated the catalogue'
            result = {'status': 'ok'}
            if method in ('star.view', 'unstar.view', 'getAlbum.view', 'getSong.view', 'getArtist.view'):
                mutation = method in ('star.view', 'unstar.view')
                if mutation:
                    params = [key for key in ('id', 'albumId', 'artistId') if key in args]
                    assert len(params) == 1
                    entity = {'id': 'song', 'albumId': 'album', 'artistId': 'artist'}[params[0]]
                    identifier = args[params[0]][0]
                    Fixture.mutation_counts[entity, identifier] = Fixture.mutation_counts.get((entity, identifier), 0) + 1
                    Fixture.mutations += 1
                    Fixture.mode_mutations += 1
                    if current == 'star_reject' or (current == 'star_partial' and Fixture.mode_mutations == 6):
                        self.reply({'subsonic-response': {'status': 'failed', 'error': {'code': 40, 'message': 'synthetic-server-password ?u=fixture&t=echo'}}})
                        return
                    Fixture.stars[entity, identifier] = method == 'star.view'
                    if current == 'star_badresponse':
                        self.reply_bytes(b'{"subsonic-response":{"status":"ok","status":"failed"}}')
                        return
                    if current == 'star_lost':
                        Fixture.lost_mutations += 1
                        self.connection.shutdown(socket.SHUT_RDWR)
                        self.connection.close()
                        return
                    if current == 'star_slow':
                        time.sleep(.15)
                else:
                    entity = {'getAlbum.view': 'album', 'getSong.view': 'song', 'getArtist.view': 'artist'}[method]
                    assert 'albumId' not in args and 'artistId' not in args
                    identifier = args['id'][0]
                    item = {'id': identifier if current != 'star_wrongid' else 'wrong-identity', 'name': 'Favorite album', 'title': 'Favorite track', 'albumId': 'targeted-album', 'songCount': 0, 'song': []}
                    if current == 'artist_persisted':
                        assert method != 'getArtist.view', 'durable artist annotation was ignored'
                    if entity == 'album' and current.startswith('album_'):
                        if current == 'album_pending':
                            time.sleep(.25)
                        first = {'id': identifier + '-clear', 'albumId': identifier, 'title': 'Later track', 'discNumber': 2, 'track': 1}
                        second = {'id': identifier + '-starred', 'albumId': identifier, 'title': 'Earlier track', 'discNumber': 1, 'track': 4, 'starred': '2026-10-25T00:00:00Z'}
                        if current == 'album_duplicate':
                            first = second.copy()
                        item.update(name='Targeted album', songCount=3 if current == 'album_details_fail' else 2, song=[first, second])
                        Fixture.stars['song', identifier + '-starred'] = True
                        if current == 'album_oversize':
                            item['ignored'] = 'x' * 300000
                    if current == 'star_badstate':
                        item['starred'] = True
                    elif Fixture.stars.get((entity, identifier), False):
                        item['starred'] = '2026-10-25T00:00:00Z'
                    result[entity] = item
                self.reply({'subsonic-response': result})
                return
            if method == 'ping.view' and current == 'offline':
                self.reply_bytes(b'{}', 503)
                return
            if method == 'ping.view':
                self.reply({'subsonic-response': result})
                return
            Fixture.calls += 1
            if current == 'slow':
                time.sleep(.15)
            if current == 'reject':
                self.reply({'subsonic-response': {'status': 'failed', 'error': {'code': 40, 'message': 'synthetic-server-password ?u=fixture&t=echo'}}})
                return
            if method == 'getScanStatus.view':
                Fixture.scans += 1
                result['scanStatus'] = {'scanning': current == 'scanning', 'lastScan': str(Fixture.scans) if current == 'scanchange' else 'fixed-marker'}
                self.reply({'subsonic-response': result})
                return
            albums, songs = (1700, 17000) if current == 'large' else (170, 1700) if current == 'bytes' else (2, 2000) if current == 'exact' else (2, 1200) if current == 'tokens' else (2, 100) if current == 'repeat' else (2, 30)
            if current in ('empty', 'deleted'):
                albums = 0
                songs = 0 if current == 'empty' else 30
            if method == 'getAlbumList2.view':
                assert args['type'] == ['starred']
                amount, offset = int(args['size'][0]), int(args['offset'][0])
                assert 0 < amount <= 500
                Fixture.byte_reduced |= current == 'bytes' and amount < 500
                entries = []
                for i in range(offset, min(albums, offset + amount)):
                    index = i if current in ('large', 'bytes', 'exact', 'repeat') else i * 2
                    entry = {'id': f'album-{index:05d}', 'name': f'Album {index:05d}' if current == 'large' else 'Shared album', 'songCount': 11 if current == 'badcount' else 10, 'starred': '2026-10-25T00:00:00Z'}
                    if current == 'bytes':
                        entry['ignored'] = 'x' * 120000
                    elif current == 'oversingle':
                        entry['ignored'] = 'x' * 300000
                    entries.append(entry)
                result['albumList2'] = {'album': entries}
            else:
                assert args['query'] == [''] and args['albumCount'] == ['0'] and args['artistCount'] == ['0']
                amount, offset = int(args['songCount'][0]), int(args['songOffset'][0])
                assert 0 < amount <= 1000
                Fixture.token_reduced |= current == 'tokens' and amount < 1000
                Fixture.exact_empty |= current == 'exact' and offset == songs
                if current == 'truncated':
                    self.reply_bytes(b'{"subsonic-response":')
                    return
                measure = current == 'large' and Fixture.large_run == 1
                if measure:
                    with Fixture.active_lock:
                        Fixture.active += 1
                        Fixture.peak_active = max(Fixture.peak_active, Fixture.active)
                        if offset == 2000 and 1000 not in Fixture.large_pages:
                            Fixture.out_of_order = True
                    if offset == 1000:
                        Fixture.first_page_started.set()
                        assert Fixture.second_page_started.wait(2), 'second ordered fetch slot did not start'
                    elif offset == 2000:
                        assert Fixture.first_page_started.wait(2), 'first ordered fetch slot did not start'

                entries = []
                for i in range(offset, min(songs, offset + amount)):
                    if current == 'incomplete' and i == 0:
                        continue
                    index = 49 if current == 'repeat' and i == 50 else i
                    entry = {'id': f'song-{index:05d}', 'title': f'Track {index:05d}', 'albumId': f'album-{index // 10:05d}', 'artist': 'Fixture artist', 'discNumber': i % 2 + 1, 'track': i % 5 + 1}
                    if current in ('large', 'exact', 'bytes') or (i in (2, 11, 25) and current != 'deleted') or (i == 11 and current == 'deleted'):
                        entry['starred'] = '2026-10-25T00:00:00.123+00:00'
                    if current == 'badstar' and i == 0:
                        entry['starred'] = True
                    if current == 'badid' and i == 0:
                        del entry['id']
                    if current == 'tokens':
                        entry.update(title='T' * 510, artist='A' * 510, coverArt='C' * 200)
                    entries.append(entry)
                result['searchResult3'] = {'song': entries}
                if measure:
                    with Fixture.active_lock:
                        Fixture.large_pages.append(offset)
                        Fixture.active -= 1
            self.reply({'subsonic-response': result})
            if current == 'large' and Fixture.large_run == 1 and method == 'search3.view' and offset == 2000:
                Fixture.second_page_started.set()
        except Exception:
            library.Fixture.failures.append('favorites fixture contract failure')
            self.reply_bytes(b'{}', 500)


def main():
    with ThreadingHTTPServer(('127.0.0.1', 0), Fixture) as server, tempfile.TemporaryDirectory(prefix='sonix-nd-favorites-') as root:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}', root], capture_output=True, text=True, timeout=180)
        server.shutdown()
        forbidden = ['synthetic-server-password', 'vault1', 'fixture user', '?u=', '&t=']
        assert not any(secret in result.stderr for secret in forbidden), 'diagnostics exposed fixture authentication'
        if result.returncode:
            print(result.stderr[-16000:], file=sys.stderr)
        print(result.stdout, end='')
        assert result.returncode == 0, f'favorites check exited {result.returncode}'
        assert not library.Fixture.failures, library.Fixture.failures
        assert Fixture.byte_reduced and Fixture.token_reduced and Fixture.exact_empty
        assert Fixture.lost_mutations == 2, "uncertain mutation was retried"
        assert Fixture.mutations > 40
        for identifier in ('write-failure-track', 'hydrated-album', 'incomplete-album', 'duplicate-album', 'oversized-album'):
            entity = 'song' if identifier == 'write-failure-track' else 'album'
            expected = 2 if identifier == 'hydrated-album' else 1
            assert Fixture.mutation_counts.get((entity, identifier)) == expected, (identifier, Fixture.mutation_counts)
        assert all(Fixture.stars.get(('album', f'selected-album-{i}')) for i in range(18))
        assert ('song', 'wrong-kind-track') not in Fixture.stars
        assert Fixture.compressed and Fixture.peak_active == 2 and Fixture.out_of_order, (Fixture.compressed, Fixture.peak_active, Fixture.out_of_order)
        metrics = re.findall(r'favorites metrics requests=(\d+).*?reuses=(\d+).*?slot_peak=(\d+).*?accepted_rows=18700', result.stderr)
        assert any(int(pages) <= 25 and int(reuses) > 0 and slots == '2' for pages, reuses, slots in metrics), metrics
        print('Favorites optimized refresh: gzip, reused sessions, two out-of-order slots and <=25 requests for 18700 rows: passed')
        print('Favorites fixture authentication, redaction, exact completion and adaptive-page contracts: passed')


if __name__ == '__main__':
    main()
