#!/usr/bin/env python3
from pathlib import Path
import hashlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit

class Fixture(BaseHTTPRequestHandler):
    failure = False
    requests = []
    reduced = False
    sync_started = threading.Event()
    release_sync = threading.Event()
    def log_message(self, *_args):
        pass
    def reply(self, value):
        body = json.dumps({'subsonic-response': {'status':'ok', **value}}, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # A bounded client request may cancel before this delayed reply.
    def do_GET(self):
        path = urlsplit(self.path)
        q = parse_qs(path.query, keep_blank_values=True)
        if path.path == '/control':
            if q['mode'] == ['sync-wait']:
                assert Fixture.sync_started.wait(5)
            else:
                Fixture.failure = q['mode'] == ['fail']
            self.reply({})
            return
        assert path.path.startswith('/base/rest/')
        assert q['u'] == ['fixture user']
        assert q['t'][0] == hashlib.md5(('synthetic-server-password' + q['s'][0]).encode()).hexdigest()
        method = path.path.rsplit('/', 1)[1]
        Fixture.requests.append(method)
        if method == 'ping.view':
            self.reply({})
        elif method == 'getScanStatus.view':
            Fixture.sync_started.set()
            assert Fixture.release_sync.wait(5)
            time.sleep(.2)
            self.reply({'scanStatus': {'scanning': False}})
        elif method == 'search3.view':
            query = q['query'][0]
            enabled = [(k,c,o,t) for k,c,o,t in [('song','songCount','songOffset',17000),('album','albumCount','albumOffset',1700),('artist','artistCount','artistOffset',1)] if int(q[c][0])]
            assert len(enabled) == 1
            kind, ck, ok, total = enabled[0]
            count, offset = int(q[ck][0]), int(q[ok][0])
            assert 0 < count <= 1000
            if Fixture.failure or query == 'broken':
                self.reply({'status':'failed', 'error':{'code':0, 'message':'QUERYSECRET synthetic-server-password ?u=fixture user&t=echo'}})
                return
            if query in ('html','invalid'):
                body = b'<html>QUERYSECRET synthetic-server-password</html>' if query=='html' else b'{malformed QUERYSECRET'
                self.send_response(200)
                self.send_header('Content-Length',str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if query == 'close':
                self.close_connection = True
                return
            if query == 'depth':
                data = 0
                for _ in range(70): data = [data]
                self.reply({'ignored':data})
                return
            if query == 'auth':
                Fixture.release_sync.set()
                self.reply({'status':'failed', 'error':{'code':40, 'message':'synthetic-server-password'}})
                return
            if query == 'slow':
                time.sleep(.2)
            if query == 'empty' or query == 's':
                total = 0
            elif query == 'eligible':
                total = 80
            elif query == 'exact':
                total = 16
            elif query == 'reduce':
                total = 6
                if count > 2:
                    self.reply({'ignored':[0]*17000, 'searchResult3':{}})
                    Fixture.reduced = True
                    return
            elif query == 'tokensingle':
                self.reply({'ignored':[0]*17000, 'searchResult3':{}})
                return
            if query.startswith('%_'):
                assert query == '%_\\ Björk QUERYSECRET'
            if query == 'repeat':
                offset = 0
            rows = [dict(id=f'remote-{kind}-{i:05}', **({'title':f'Track {i:05}'} if kind=='song' else {'name':f'{kind} {i:05}'})) for i in range(offset,min(total,offset+count))]
            if kind == 'artist':
                for row in rows: row['albumCount'] = 1
                if query == 'eligible':
                    for row in rows:
                        i = int(row['id'].rsplit('-', 1)[1])
                        row['albumCount'] = int(i % 3 == 0)
                        if i == 5: del row['albumCount']
            if kind == 'song':
                for row in rows:
                    row['albumId'] = 'remote-album'
                    row['album'] = 'Remote album'
            if query == 'longid': rows = [dict(id='x'*256,title='invalid') ]
            self.reply({'searchResult3':{kind:rows}})
        elif method == 'getArtist.view':
            identifier = q['id'][0]
            if identifier == 'remote-artist-00005':
                self.reply({'artist':dict(id=identifier)})
            elif identifier == 'oversized':
                self.reply({'ignored':[0]*17000})
            else:
                assert identifier == 'new-artist'
                self.reply({'artist':dict(id=identifier,album=[dict(id=f'new-{i}',name=f'New album {i:03}') for i in range(100)])})
        elif method == 'getAlbum.view':
            identifier = q['id'][0]
            if identifier != 'new-album' and not (getattr(Fixture, 'discovery_synced', False) and identifier == 'album-00000'):
                self.reply({'status':'failed','error':{'code':70,'message':'QUERYSECRET'}})
            else:
                self.reply({'album':dict(id=identifier,song=[dict(id=f'new-song-{i}',title=f'Track {i}',discNumber=2-i//6,track=6-i%6) for i in range(12)])})
        else:
            raise AssertionError('unexpected request')

def main():
    with ThreadingHTTPServer(('127.0.0.1',0),Fixture) as server, tempfile.TemporaryDirectory(prefix='sonix-nd-search-') as root:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        result=subprocess.run([sys.argv[1],f'http://127.0.0.1:{server.server_port}',root],capture_output=True,text=True,timeout=120)
        server.shutdown()
        forbidden=['synthetic-server-password','fixture user','QUERYSECRET','?u=','&t=']
        assert not any(x in result.stderr for x in forbidden),'sensitive diagnostics'
        print(result.stdout,end='')
        if result.returncode:
            print(result.stderr[-6000:],file=sys.stderr)
        assert result.returncode==0, f'search check exited {result.returncode}'
        metadata_files = list(Path(root).rglob('*.db'))
        metadata_files += list(Path(root).rglob('.playqueue-*'))
        metadata_files += list(Path(root).rglob('search-??????'))
        for metadata in metadata_files:
            data = metadata.read_bytes()
            assert not any(word.encode() in data for word in forbidden), 'authentication/raw error persisted in metadata'
        assert Fixture.reduced
        assert set(Fixture.requests)<= {'ping.view','search3.view','getArtist.view','getAlbum.view','getScanStatus.view'}
        assert Fixture.requests.count('getScanStatus.view') == 1  # Only the explicit sync race.
        print('Real loopback authentication, API prefix, request exclusions and redaction: passed')
if __name__=='__main__':
    main()
