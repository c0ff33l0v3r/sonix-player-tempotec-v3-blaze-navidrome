#!/usr/bin/env python3
import hashlib, json, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
class Fixture(BaseHTTPRequestHandler):
    mode = 'normal'
    requests = []
    def log_message(self, *_): pass
    def reply(self, data):
        body = data if isinstance(data,bytes) else json.dumps({'subsonic-response':{'status':'ok',**data}},ensure_ascii=False).encode()
        self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError,ConnectionResetError): pass
    def do_GET(self):
        path=urlsplit(self.path); q=parse_qs(path.query,keep_blank_values=True)
        if path.path=='/control': Fixture.mode=q['mode'][0]; self.reply({}); return
        assert path.path.startswith('/base/rest/')
        assert q['u']==['fixture user'] and q['t'][0]==hashlib.md5(('synthetic-server-password'+q['s'][0]).encode()).hexdigest()
        method=path.path.rsplit('/',1)[1]; Fixture.requests.append((method,q.get('type',[''])[0]))
        if method=='ping.view': self.reply({}); return
        if method=='getAlbumList2.view':
            mode=Fixture.mode; size=int(q['size'][0]); offset=int(q['offset'][0]); kind=q['type'][0]
            if mode=='slow': time.sleep(.3)
            assert kind in ('newest','recent','frequent','random') and 0<size<=50
            if mode=='auth': self.reply({'status':'failed','error':{'code':40,'message':'SERVERSECRET'}}); return
            if mode=='api': self.reply({'status':'failed','error':{'code':0,'message':'SERVERSECRET'}}); return
            if mode=='malformed': self.reply(b'{malformed SERVERSECRET'); return
            if mode=='depth':
                value=0
                for _ in range(70): value=[value]
                self.reply({'albumList2':{'album':[]},'extra':value}); return
            if mode=='oversize-one' or (mode=='reduce' and size>2):
                self.reply({'albumList2':{'album':[]},'padding':'X'*270000}); return
            total=0 if mode=='empty' else 32 if mode=='multiple' else 3 if mode=='small-random' else 1700
            if kind=='random':
                assert offset==0
                offset=100
                total=offset+min(size,total)
            if mode=='repeat': offset=0
            rows=[{'id':f'{kind}-{i}','name':f'Björk album {i}','artist':'Artist','coverArt':f'cover-{i}'} for i in range(offset,min(total,offset+size))]
            if mode=='duplicate' and len(rows)>1: rows[1]['id']=rows[0]['id']
            if mode=='invalid' and rows: rows[0]['id']=''
            self.reply({'albumList2':{'album':rows}}); return
        if method=='getAlbum.view':
            album=q['id'][0]; count=65 if album=='large' else 64 if album=='boundary' else 12
            if album=='oversized': self.reply({'album':{'id':album,'name':'Large','song':[]},'padding':'X'*270000}); return
            rows=[{'id':f'fresh-track-{i}' if album=='append-fault' else f'track-{i}','title':f'Track {i}','artist':'Artist','albumId':album,'album':'Album','discNumber':1+i//8,'track':i%8+1,'duration':200} for i in range(count)]
            self.reply({'album':{'id':'other' if album=='wrong' else album,'name':'Album','artist':'Artist','coverArt':'discovery-cover','songCount':count,'song':rows}}); return
        raise AssertionError(method)
if __name__=='__main__':
    server=ThreadingHTTPServer(('127.0.0.1',0),Fixture)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='discovery-fixture-') as root:
            result=subprocess.run([sys.argv[1],f'http://127.0.0.1:{server.server_port}',root,*sys.argv[2:]],capture_output=True,text=True,timeout=120)
            assert 'SERVERSECRET' not in result.stdout+result.stderr
            print(result.stdout,end='')
            if result.returncode: print(result.stderr[-6000:],end='',file=sys.stderr)
            if result.returncode: raise SystemExit(result.returncode)
            kinds={kind for method,kind in Fixture.requests if method=='getAlbumList2.view'}
            assert kinds == {'newest','recent','frequent','random'} if len(sys.argv)==2 else kinds <= {'newest','recent','frequent','random'}
    finally: server.shutdown(); server.server_close()
