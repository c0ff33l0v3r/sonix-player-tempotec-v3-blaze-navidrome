#!/usr/bin/env python3
"""Real loopback original-download fixtures; no player build or audio output."""
import http.server
import hashlib
import io
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import threading
import time
import urllib.parse
import wave

player = Path(__file__).resolve().parent.parent
compiler = shlex.split(os.environ.get("CC", "cc"))
extra = shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", ""))
audio = io.BytesIO()
with wave.open(audio, "wb") as output:
    output.setnchannels(1)
    output.setsampwidth(2)
    output.setframerate(44100)
    output.writeframes(b"\0\0" * 44100)
payload = audio.getvalue()
mode = "success"
requests = []
request_ids = []
card = None

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        assert parsed.path in ("/prefix/rest/download.view", "/prefix/rest/stream.view", "/prefix/rest/getAlbum.view", "/prefix/rest/getSong.view", "/prefix/rest/final.view")
        assert query["id"][0] in ("track/1?", "track/2?", "album")
        assert query["u"] == ["fixture user"]
        assert len(query["t"][0]) == len(query["s"][0]) == 32
        assert query["t"][0] == hashlib.md5(("synthetic-download-password" + query["s"][0]).encode()).hexdigest()
        if parsed.path.endswith("download.view"):
            assert "format" not in query and query["v"] == ["1.16.1"] and query["c"] == ["SonixPlayer"]
        requests.append(parsed.path)
        request_ids.append(query["id"][0])
        body = payload
        if mode == "preempt_headers":
            (card / "entered").write_text("entered")
            time.sleep(2)
        if mode == "redirect" and parsed.path.endswith("download.view"):
            self.send_response(302)
            self.send_header("Location", self.path.replace("download.view", "final.view"))
            self.end_headers()
            return
        if parsed.path.endswith("getAlbum.view"):
            body = b'{"subsonic-response":{"status":"ok","album":{"id":"album","displayAlbumArtist":"Various Artists","song":[{"id":"track/1?","discNumber":1},{"id":"track/2?","discNumber":2}]}}}'
        if parsed.path.endswith("getSong.view"):
            import json
            exact = query["id"][0]
            number = 1 if exact == "track/1?" else 2
            body = json.dumps({"subsonic-response": {"status": "ok", "song": {
                "id": "wrong-song" if mode == "playlist_badmeta" else exact,
                "title": f"Track {number}", "artist": f"Performer {number}",
                "album": "Album", "albumId": "album", "track": number, "discNumber": number}}}).encode()
        if mode == "large":
            body = payload + b'\0' * (9 * 1024 * 1024)
        if mode == "error" or (mode == "partial_retry" and query["id"] == ["track/1?"] and request_ids.count("track/1?") == 1):
            body = b'<subsonic-response status="failed"><error message="synthetic-download-password"/></subsonic-response>'
        if mode in ("html", "json"):
            body = b'<html>synthetic-download-password</html>' if mode == "html" else b'{"error":"synthetic-download-password"}'
        self.send_response(401 if mode == "auth" else 200)
        self.send_header("Content-Type", "text/xml" if mode == "error" else "audio/wav")
        if mode in ("chunk", "chunk_bad"):
            self.send_header("Transfer-Encoding", "chunked")
        elif mode not in ("unknown", "unknown_limit"):
            self.send_header("Content-Length", str(2147483648 if mode == "oversize" else len(body) + (99 if mode == "truncated" else 0)))
        if mode == "gzip":
            self.send_header("Content-Encoding", "gzip")
        self.end_headers()
        try:
            if mode == "clear_active" and query["id"] == ["track/2?"]:
                self.wfile.write(body[:2048])
                self.wfile.flush()
                (card / "entered").write_text("entered")
                time.sleep(2)
                self.wfile.write(body[2048:])
            elif mode in ("pause", "cancel", "preempt") or (mode == "resume" and len(requests) == 1):
                self.wfile.write(body[:2048])
                self.wfile.flush()
                time.sleep(2)
            elif mode in ("chunk", "chunk_bad"):
                self.wfile.write(f'{len(body):x}\r\n'.encode() + body + b'\r\n')
                if mode == "chunk": self.wfile.write(b'0\r\n\r\n')
            elif mode != "oversize":
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

with tempfile.TemporaryDirectory(prefix="sonix-download-check-") as temporary:
    root = Path(temporary)
    sqlite = player / "build_host/system/db/sqlite3_impl.o"
    if not sqlite.exists():
        sqlite = root / "sqlite.o"
        subprocess.run(compiler + ["-std=gnu11", "-w", "-I", str(player), "-c", str(player / "src/system/db/sqlite3_impl.c"), "-o", str(sqlite)] + extra, check=True)
    sources = [
        "tools/navidrome_downloadcheck.c", "tools/navidrome_test_support.c",
        "src/system/streaming/navidromedownloads.c", "src/system/streaming/navidromedownloadstore.c",
        "src/system/streaming/navidromedownloadfile.c", "src/system/streaming/navidromeaudio.c",
        "src/system/streaming/navidromeapi.c", "src/system/streaming/navidromecrypto.c",
        "src/system/streaming/navidromestore.c", "src/system/streaming/navidromeplayliststore.c",
        "src/system/streaming/streamturn.c", "src/system/decode/growfile.c",
        "src/system/core/json.c", "src/system/core/md5.c", "src/system/core/sha256.c",
        "src/system/net/http.c", "src/system/net/tls.c", "src/system/net/gzip.c", "src/gb/miniz/miniz.c", "src/system/image/png_scaled.c",
    ]
    binary = root / "downloadcheck"
    compile_args = compiler + ["-std=gnu11", "-DHOST_BUILD", "-DLV_CONF_INCLUDE_SIMPLE", "-D_FILE_OFFSET_BITS=64", "-DNAVIDROME_DOWNLOAD_PUBLISH_TEST", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-ffunction-sections", "-fdata-sections", "-I", str(player), "-I", str(player / "lvgl")] + extra + [str(player / source) for source in sources] + [str(sqlite), "-Wl,--gc-sections,--wrap=write,--wrap=fsync,--wrap=fstatvfs64,--wrap=nd_pl_play_window", "-pthread", "-ldl", "-lm", "-lresolv"]
    subprocess.run(compile_args + ["-o", str(binary)], check=True)
    limited = root / "downloadcheck-limit"
    subprocess.run(compile_args + ["-DND_DL_MAX_BYTES=65536", "-o", str(limited)], check=True)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        fixtures = ("clear_history", "clear_active", "deleted_again", "deleted_history", "deleted_retry", "folders", "success", "same_title", "offline", "active_cache", "unknown", "collision", "error", "json", "html", "truncated", "oversize", "unsupported", "pause", "cancel", "preempt", "preempt_headers", "resume", "single_multi", "current", "current_remote", "recover", "after_rename", "readonly", "reject_post", "large", "chunk", "chunk_bad", "gzip", "unknown_limit", "auth", "write_fail", "flush_fail", "full", "favorites", "selection", "case_collision", "capture_refresh", "large_batch", "fallback", "modified", "partial_retry", "root", "playlist", "playlist_remote", "playlist_stale", "playlist_changed", "playlist_deleted", "playlist_large", "playlist_badmeta", "playlist_offline")
        selected = os.environ.get("NAVIDROME_DOWNLOAD_CASES")
        if selected:
            chosen = tuple(selected.split(","))
            assert all(name in fixtures for name in chosen)
            fixtures = chosen
        for fixture in fixtures:
            mode = fixture
            requests.clear()
            request_ids.clear()
            card = root / fixture
            card.mkdir()
            result = subprocess.run([str(limited if fixture == "unknown_limit" else binary), f"http://127.0.0.1:{server.server_port}/prefix", str(card), fixture], capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, f"{fixture}: {result.stdout}\n{result.stderr}"
            assert "synthetic-download-password" not in result.stdout + result.stderr
            if fixture in ("folders", "unsupported", "readonly", "large_batch", "playlist_large", "playlist_stale", "playlist_changed", "playlist_deleted"):
                assert not requests
            if fixture == "playlist":
                assert request_ids == ["track/1?", "track/2?"]
            if fixture == "playlist_badmeta":
                assert requests and all(path.endswith("getSong.view") for path in requests)
            if fixture in ("offline", "active_cache", "fallback", "playlist_offline"):
                assert requests == ["/prefix/rest/stream.view"] * 2
            if fixture in ("partial_retry", "deleted_again", "deleted_history", "deleted_retry"):
                assert request_ids == ["track/1?", "track/2?", "track/1?"]
            if fixture == "large_batch":
                print(next(line for line in result.stdout.splitlines() if line.startswith("bounded downloads:")), flush=True)
            print(f"PASS {fixture}", flush=True)
        print(f"{len(fixtures)} original-download loopback fixtures passed")
    finally:
        server.shutdown()
        server.server_close()
