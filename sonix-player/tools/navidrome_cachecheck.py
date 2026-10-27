#!/usr/bin/env python3
"""Synthetic cards only; build the production cache walker, never the player."""
import pathlib
import subprocess
import tempfile
import os
ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='sonix-polish-cache-') as temp:
    root = pathlib.Path(temp)
    binary = root / 'check'
    subprocess.run([os.getenv('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-DHOST_BUILD', '-DLV_CONF_INCLUDE_SIMPLE', '-I.', '-Ilvgl',
                    '-ffunction-sections', '-fdata-sections', 'tools/navidrome_cachecheck.c',
                    '-Wl,--gc-sections,--wrap=unlinkat',
                    '-pthread', '-o', str(binary)], cwd=ROOT, check=True)
    card = root / 'card'
    base = card / '.local/navidrome'
    modern = base / '.cache'
    scope = 'a' * 32
    owned = []
    kept = []
    def put(path, size, disposable=False):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'x' * size)
        (owned if disposable else kept).append(path)
    for layout in (base, modern):
        for ext in ('.mp3', '.complete', '.tmp', '.flac', '.dsf'):
            put(layout / 'audio' / scope / ('b' * 32 + ext), 11, True)
        for ext in ('.img', '.img.tmp'):
            put(layout / 'art' / ('c' * 32 + ext), 7, True)
        put(layout / 'audio' / scope / 'unknown.flac', 50)
        put(layout / 'audio' / 'unknown' / ('d' * 32 + '.mp3'), 50)
        put(layout / 'art' / 'unrelated.img', 50)
    for name in ('library.db', 'favorites.db', 'library.db-wal'):
        put(modern / name, 19)
    for name in ('playlists-scope.db', 'downloads-scope.db', 'lyrics-scope.db'):
        put(base / name, 23)
    put(card / 'Music/Navidrome/Artist/Album/01.flac', 31)
    put(card / 'credentials.cache', 37)
    put(card / '.local/navidrome-debug-1.log', 41)
    outside = root / 'outside'
    put(outside / ('e' * 32 + '.mp3'), 99)
    (modern / 'audio' / ('f' * 32)).symlink_to(outside, target_is_directory=True)
    (modern / 'art' / ('e' * 32 + '.img')).symlink_to(kept[-1])
    def scan(clear=False, fail=None, path=None):
        args = [str(binary), str(path or modern / 'library.db'), str(int(clear))]
        if fail: args.append(fail)
        return tuple(map(int, subprocess.check_output(args, text=True, cwd=root).split()))
    assert scan() == (1, 1, 110, 28)
    assert scan(path=pathlib.Path("card/.local/navidrome/.cache/library.db")) == (1, 1, 110, 28)
    # Keep these as strings: pathlib normalizes away the explicit dot components.
    for path in ("./card/.local/navidrome/.cache/library.db",
                 "././card/./.local/navidrome/.cache/library.db"):
        assert scan(path=path) == (1, 1, 110, 28)
    result = tuple(map(int, subprocess.check_output([str(binary), str(modern / 'library.db'), '2'], text=True).split()))
    assert result[0] == 0 and any(path.exists() for path in owned) and any(not path.exists() for path in owned)
    assert all(path.exists() for path in kept)
    for path in owned:
        path.write_bytes(b'x' * (7 if '/art/' in str(path) else 11))
    fail = 'b' * 32 + '.mp3'
    assert scan(True, fail) == (0, 1, 22, 0)
    assert all(path.exists() for path in kept)
    assert scan(True, path="./card/.local/navidrome/.cache/library.db") == (1, 1, 0, 0)
    assert all(path.exists() for path in kept)
    assert not any(path.exists() for path in owned)
    assert scan(path=base / 'library.db') == (1, 1, 0, 0)
    alias = root / 'alias'
    alias.symlink_to(card, target_is_directory=True)
    assert scan(True, path=alias / '.local/navidrome/.cache/library.db') == (0, 0, 0, 0)
    assert scan(True, path='./alias/.local/navidrome/.cache/library.db') == (0, 0, 0, 0)
    assert scan(True, path=modern / '../.cache/library.db') == (0, 0, 0, 0)
    print('cache walker: current/legacy byte totals, audio/art/temp/completion files, partial failure and retry, in-progress context cancellation, symlink/parent escape rejection, retained metadata/Favorites/playlists/downloads/lyrics/credentials/log sentinels: passed')
