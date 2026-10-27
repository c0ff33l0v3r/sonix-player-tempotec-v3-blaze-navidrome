#!/usr/bin/env python3
from http.server import ThreadingHTTPServer
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile
import threading
from urllib.parse import parse_qs, urlsplit
from navidrome_searchcheck import Fixture


class Actions(Fixture):
    stars = []

    def do_GET(self):
        parsed = urlsplit(self.path)
        query = parse_qs(parsed.query, keep_blank_values=True)
        method = parsed.path.rsplit('/', 1)[-1]
        if method in ('star.view', 'unstar.view'):
            assert query['u'] == ['fixture user']
            assert query['t'] == [hashlib.md5(('synthetic-server-password' + query['s'][0]).encode()).hexdigest()]
            exact = query.get('albumId', query.get('id'))[0]
            self.stars.append((method, exact))
            if exact == 'batch-failed-1':
                self.reply({'status': 'failed', 'error': {'code': 0, 'message': 'synthetic-server-password'}})
            elif exact == 'batch-uncertain-1':
                self.close_connection = True
            else:
                self.reply({})
        elif method == 'getSong.view' and query.get('id', [''])[0].startswith('batch-'):
            self.reply({'song': {'id': query['id'][0], 'title': 'Captured favorite', 'albumId': 'batch-album'}})
        elif method == 'getAlbum.view' and query.get('id') == ['bad-album']:
            Fixture.requests.append(method)
            self.reply({'album': {'id': 'bad-album', 'songCount': 2, 'song': [{'id': 'one-only', 'title': 'Incomplete'}]}})
        elif method == 'getAlbum.view' and query.get('id') == ['retained-large']:
            Fixture.requests.append(method)
            self.reply({'album': {'id': 'retained-large', 'name': 'Retained complete album', 'songCount': 100,
                                 'song': [{'id': f'retained-{i:03}', 'title': f'Child {i:03}', 'track': i + 1} for i in range(100)]}})
        else:
            super().do_GET()


def main():
    with ThreadingHTTPServer(('127.0.0.1', 0), Actions) as server, tempfile.TemporaryDirectory(prefix='sonix-nd-actions-') as root:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}', root, *sys.argv[2:]], text=True, capture_output=True, timeout=240)
        server.shutdown()
        forbidden = ['synthetic-server-password', 'fixture user', 'QUERYSECRET', '?u=', '&t=']
        assert not any(value in result.stderr for value in forbidden), 'sensitive diagnostics'
        print(result.stdout, end='')
        if result.returncode:
            print(result.stderr[-6000:], file=sys.stderr)
        assert result.returncode == 0, f'action fixture exited {result.returncode}'
        assert not any(method == 'search3.view' for method in Fixture.requests), 'single/selected action enumerated root Search'
        assert Actions.stars == ([] if len(sys.argv) > 2 else [('star.view', exact) for exact in
            ('new-album', 'batch-failed-0', 'batch-failed-1', 'batch-uncertain-0', 'batch-uncertain-1', 'batch-uncertain-0')])
        for path in Path(root).rglob('*.db'):
            data = path.read_bytes()
            assert not any(value.encode() in data for value in forbidden), 'authentication/error persisted'
        print('Protected action HTTP, no root Search replay, exact album favorite and diagnostic/store redaction: passed')


if __name__ == '__main__':
    main()
