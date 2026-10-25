#!/usr/bin/env python3
"""Real server metadata and real playback coordinator with synthetic audio output."""
from http.server import ThreadingHTTPServer
import subprocess
import sys
import threading
from navidrome_searchcheck import Fixture

with ThreadingHTTPServer(('127.0.0.1', 0), Fixture) as server:
    threading.Thread(target=server.serve_forever, daemon=True).start()
    result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}/base'],
                            capture_output=True, text=True, timeout=120)
    server.shutdown()
    assert not any(secret in result.stderr for secret in
                   ['synthetic-server-password', 'fixture user', 'QUERYSECRET', '?u=', '&t='])
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr[-6000:], file=sys.stderr)
    assert result.returncode == 0, f'search playback check exited {result.returncode}'
    assert set(Fixture.requests) <= {'search3.view', 'getAlbum.view'}
    assert 'search3.view' in Fixture.requests and 'getAlbum.view' in Fixture.requests
    print('Real loopback search/album capture through playback and shuffle: passed')
