"""Real synthetic PCM/FLAC/MP3 responses; no real server, account or audio output."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import wave
from urllib.parse import parse_qs, urlsplit


class Fixture(BaseHTTPRequestHandler):
    audio = {}
    def log_message(self, *args):
        pass

    def do_GET(self):
        parsed = urlsplit(self.path)
        q = parse_qs(parsed.query)
        assert q['id'] == ['opaque:/track ?+'] and q['format'] == ['raw']
        assert q['t'] == [hashlib.md5(('synthetic-server-password' + q['s'][0]).encode()).hexdigest()]
        route = parsed.path.split('/')[1]
        body = self.audio.get(route, self.audio['flac'])
        if route == 'invalid':
            body = b'<subsonic-response status="failed">synthetic-server-password</subsonic-response>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/xml' if route == 'invalid' else 'application/octet-stream')
        if route not in ('unknown', 'unknownswitch'):
            self.send_header('Content-Length', str(len(body) + (100000 if route == 'cut' else 0)))
        self.end_headers()
        if route in ('detach', 'switch', 'unknownswitch'):
            self.wfile.write(body[:16384]); self.wfile.flush()
            time.sleep(2)
            return
        try:
            for offset in range(0, len(body), 16384):
                self.wfile.write(body[offset:offset+16384])
                self.wfile.flush()
                time.sleep(.001)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    if not shutil.which('ffmpeg'):
        raise SystemExit('Requires ffmpeg to generate synthetic decoder fixtures; no check ran.')
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='sonix-audio-fixture-') as directory:
        root = Path(directory)
        with wave.open(str(root/'noise.wav'), 'wb') as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(44100)
            wav.writeframes(random.Random(1729).randbytes(44100*20*2))
        for ext, codec in [('flac', 'flac'), ('mp3', 'libmp3lame')]:
            command = ['ffmpeg','-v','error','-i',str(root/'noise.wav'),'-c:a',codec]
            if ext == 'mp3':
                command += ['-b:a','320k']
            subprocess.run(command+[str(root/f'noise.{ext}')], check=True, timeout=30)
        Fixture.audio = {ext:(root/f'noise.{ext}').read_bytes() for ext in ('wav','flac','mp3')}
        with ThreadingHTTPServer(('127.0.0.1',0),Fixture) as server:
            threading.Thread(target=server.serve_forever,daemon=True).start()
            for route, mode in [('flac','progressive'),('mp3','progressive'),('wav','complete'),
                                ('unknown','progressive'),('cut','cut'),('invalid','invalid'),('cancel','cancel'),
                                ('protected','protected'),('evict','evict'),('detach','detach'),
                                ('publish','publish'),('network','network'),('prefetch','prefetch'),
                                ('switch','switch'),('unknownswitch','switch'),('turn','turn'),
                                ('write-fail','write-fail'),('close-fail','close-fail'),
                                ('marker-fail','marker-fail'),('free-loss','free-loss')]:
                card=root/route
                (card/'.local/navidrome/.cache').mkdir(parents=True)
                completed=subprocess.run([binary,f'http://127.0.0.1:{server.server_port}/{route}',str(card),mode],
                                         capture_output=True,text=True,timeout=30)
                output=completed.stdout+completed.stderr
                assert 'synthetic-server-password' not in output and 'opaque:/' not in output
                if completed.returncode:
                    raise AssertionError(f'{route}: {output}')
            server.shutdown()
    print('real MP3/FLAC progressive decode, complete WAV, unknown length, offline/read-only reuse, truncation, API-error rejection, budgets/eviction, publication/invalidation, descriptor drain and cancellation passed')


if __name__ == '__main__':
    main()
