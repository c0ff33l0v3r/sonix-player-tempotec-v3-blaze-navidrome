import http.server
import json
import subprocess
import sys
import threading
import urllib.parse

class Fixture(http.server.BaseHTTPRequestHandler):
    playlists = {
        'source': {'name': 'Server', 'ids': ['A', 'B', 'A']},
        'readonly': {'name': 'Read only', 'ids': ['A'], 'readonly': True},
        'large': {'name': 'Large', 'ids': [f'track-{i:05d}' for i in range(1000)]},
    }
    fail_rename = lose_create = False
    post = True
    def log_message(self, *_):
        pass
    def do_GET(self):
        self.handle_api(False)
    def do_POST(self):
        self.handle_api(True)
    def playlist(self, key):
        p = self.playlists[key]
        return {'id': key, 'name': p['name'], 'songCount': len(p['ids']),
                'readonly': p.get('readonly', False), 'comment': 'preserved',
                'entry': [{'id': i, 'title': ('Metadata ' * 35 if i.startswith('track-') else i), 'artist': 'Fixture artist', 'albumId': 'album', 'duration': 200} for i in p['ids']]}
    def handle_api(self, posted):
        assert self.path.startswith('/prefix/rest/'), self.path
        parsed = urllib.parse.urlsplit(self.path)
        method = parsed.path.rsplit('/', 1)[1].removesuffix('.view')
        params = urllib.parse.parse_qs(parsed.query)
        assert params['u'] == ['fixture-user'] and 't' in params and 's' in params
        if posted:
            assert self.headers['Content-Type'].startswith('application/x-www-form-urlencoded')
            body = self.rfile.read(int(self.headers['Content-Length']))
            params.update(urllib.parse.parse_qs(body.decode(), keep_blank_values=True))
        result = {'status': 'ok'}
        if method == 'getOpenSubsonicExtensions':
            result['openSubsonicExtensions'] = [{'name': 'formPost', 'versions': [1]}] if self.post else []
        elif method == 'getPlaylists':
            result['playlists'] = {'playlist': [dict(id=k, name=p['name'], readonly=p.get('readonly',False)) for k,p in self.playlists.items()]}
        elif method == 'getPlaylist':
            key = params['id'][0]
            if key not in self.playlists:
                result = {'status':'failed','error':{'code':70}}
            else:
                result['playlist'] = self.playlist(key)
        elif method == 'createPlaylist':
            assert posted
            key = params.get('playlistId', [''])[0]
            if not key:
                key = f'created-{len(self.playlists)}'
                self.playlists[key] = {'name': params['name'][0], 'ids': []}
            assert not self.playlists[key].get('readonly')
            self.playlists[key]['ids'] = params.get('songId', [])
            if Fixture.lose_create:
                Fixture.lose_create = False
                self.close_connection = True
                return
            result['playlist'] = self.playlist(key)
        elif method == 'updatePlaylist':
            assert posted
            if Fixture.fail_rename:
                Fixture.fail_rename = False
                result = {'status':'failed','error':{'code':0}}
            else:
                self.playlists[params['playlistId'][0]]['name'] = params['name'][0]
        elif method == 'fixtureChange':
            self.playlists['source']['ids'] = ['A']
        elif method == 'fixtureFailRename':
            Fixture.fail_rename = True
        elif method == 'fixtureLoseCreate':
            Fixture.lose_create = True
        elif method == 'fixtureNoPost':
            Fixture.post = False
        elif method == 'fixtureYesPost':
            Fixture.post = True
        elif method == 'fixtureDeleteSource':
            del self.playlists['source']
        elif method == 'fixtureAuth':
            result = {'status':'failed','error':{'code':40,'message':'fixture-secret ' + params['t'][0]}}
        elif method == 'fixtureOversize':
            result['padding'] = 'x' * (2 * 1024 * 1024)
        elif method == 'fixtureTokens':
            result['tokens'] = [0] * 131073
        else:
            raise AssertionError(method)
        data = json.dumps({'subsonic-response': result}, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}/prefix'], capture_output=True, text=True)
    assert 'fixture-secret' not in result.stderr and 'fixture-user' not in result.stderr
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr, file=sys.stderr)
    sys.exit(result.returncode)
finally:
    server.shutdown()
    server.server_close()
