#!/usr/bin/env python3
"""Production finite GET/POST/setup fixtures; no real credentials or audio device."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import time

PLAYER=Path(__file__).resolve().parents[1]


class Fixture(BaseHTTPRequestHandler):
    def log_message(self,*args):
        pass

    def do_GET(self):
        self.respond()

    def do_POST(self):
        if self.path.startswith('/cancel-post-write/'):
            time.sleep(.5)
            return
        self.rfile.read(int(self.headers.get('Content-Length','0')))
        self.respond()

    def respond(self):
        mode=self.path.split('/')[1]
        try:
            if mode in ('cancel-header','cancel-tls'):
                time.sleep(.5)
                return
            if mode=='deadline-header':
                for byte in b'HTTP/1.1 200 OK\r\nX-Trickle: unfinished':
                    self.wfile.write(bytes([byte])); self.wfile.flush(); time.sleep(.02)
                return
            if mode.startswith('post-') and mode!='post-final':
                self.send_response(int(mode[5:])); self.send_header('Location','/post-final/rest/createPlaylist.view'); self.send_header('Content-Length','0'); self.end_headers(); return
            if mode in ('cancel-body','long-body'):
                self.send_response(200); self.send_header('Content-Length','8'); self.end_headers()
                self.wfile.write(b'x'); self.wfile.flush(); time.sleep(.4)
                self.wfile.write(b'xxxxxxx'); return
            body=json.dumps({'subsonic-response':{'status':'ok','method':self.command}}).encode()
            self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
        except (BrokenPipeError,ConnectionResetError):
            pass


def main():
    with tempfile.TemporaryDirectory(prefix='sonix-transport-') as directory:
        binary=str(Path(directory)/'check')
        sources=['tools/navidrome_transportcheck.c','src/system/device/workactivity.c','src/system/streaming/navidromeapi.c','src/system/streaming/navidromecrypto.c','src/system/core/json.c','src/system/core/md5.c','src/system/net/http.c','src/system/net/tls.c','src/system/net/gzip.c','src/system/image/png_scaled.c','src/gb/miniz/miniz.c']
        command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O1','-g','-Wall','-Wextra','-ffunction-sections','-fdata-sections','-DHOST_BUILD','-DHTTP_FINITE_DEADLINE_MS=200','-DLV_CONF_INCLUDE_SIMPLE','-I.','-Ilvgl']+shlex.split(os.environ.get('NAVIDROME_CHECK_FLAGS',''))+sources+['-Wl,--gc-sections,--wrap=getaddrinfo,--wrap=connect,--wrap=poll,--wrap=send,--wrap=tls_client_wait','-pthread','-ldl','-lresolv','-lm','-o',binary]
        subprocess.run(command,cwd=PLAYER,check=True)
        with ThreadingHTTPServer(('127.0.0.1',0),Fixture) as server:
            threading.Thread(target=server.serve_forever,daemon=True).start()
            cases=['dns-http','dns-https','tls-fail','cancel-dns','cancel-connect','cancel-tls','cancel-header','cancel-body','cancel-post-write','deadline-header','long-body','post-301','post-302','post-303','post-307','post-308']
            for mode in cases:
                scheme='https' if 'tls' in mode or mode=='dns-https' else 'http'
                result=subprocess.run([binary,mode,f'{scheme}://127.0.0.1:{server.server_port}/{mode}'],capture_output=True,text=True,timeout=10)
                assert not result.returncode,(mode,result.stdout,result.stderr)
                assert 'synthetic-password' not in result.stderr and '?u=' not in result.stderr
                print(result.stdout.strip(),flush=True)
            server.shutdown()


if __name__=='__main__':
    main()
