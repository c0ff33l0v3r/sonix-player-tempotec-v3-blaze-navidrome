#!/usr/bin/env python3
"""Production guarded CGI and generated document-root loopback checks.

Uses a small HTTP/CGI bridge, not target thttpd. Executes no mounts, killall,
server startup/shutdown, power commands, or external network requests.
"""
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlencode, urlsplit

player = Path(__file__).resolve().parent.parent
cc = shlex.split(os.environ.get("CC", "cc"))
extra = shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", ""))
with tempfile.TemporaryDirectory(prefix="sonix-wifi-guard-") as tmp:
    root = Path(tmp)
    sqlite = root / "sqlite.o"
    subprocess.run(cc + ["-std=gnu11", "-O2", "-w", "-I", str(player), "-c",
                         str(player / "src/system/db/sqlite3_impl.c"), "-o", str(sqlite)] + extra, check=True)
    binary = root / "wificheck"
    subprocess.run(cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                         "-ffunction-sections", "-fdata-sections", "-I", str(player),
                         '-DFP_CARD_CONTROL="' + str(root / "control") + '"'] + extra + [
        str(player / "tools/filepublicationwificheck.c"),
        str(player / "src/system/net/webcgi.c"),
        str(player / "src/system/library/filepublication.c"),
        str(player / "src/system/library/filepublicationcard.c"), str(sqlite),
        "-Wl,--gc-sections", "-pthread", "-ldl", "-lm", "-o", str(binary)], check=True)
    def control(*args):
        return subprocess.check_output([str(binary), *map(str, args)], text=True, timeout=5).strip()
    card = root / "card"
    album = card / "album"
    album.mkdir(parents=True)
    ordinary = b"ordinary complete data\x00" * 15000
    (album / "ordinary.flac").write_bytes(ordinary)
    (album / "pending.flac").write_bytes(b"partial")
    epoch = control("root", card)
    control("pending", album)
    control("enable")  # A partial journal does not block transfer admission.
    document = Path(control("webroot").splitlines()[-1])
    endpoints = {"sonix-list", "list", "download", "upload", "create", "move", "delete"}
    assert set(p.name for p in document.iterdir()) <= endpoints | {"fonts", "hostname", "index.html", "test.conf"}
    config = (document / "test.conf").read_text()
    assert "cgipat=list|upload|download|create|move|delete|sonix-list\n" in config
    assert "**.cgi" not in config
    page = (document / "index.html").read_text()
    assert 'lister = "list"' not in page and "application/octet-stream" in page

    def environment(method, query, body, content_type):
        return dict(os.environ, GATEWAY_INTERFACE="CGI/1.1", REQUEST_METHOD=method,
                    QUERY_STRING=query, CONTENT_LENGTH=str(len(body)), CONTENT_TYPE=content_type)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass
        def serve(self):
            url = urlsplit(self.path)
            name = url.path.removeprefix("/")
            if name not in endpoints or not (document / name).exists():
                self.send_error(404)
                return
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            env = environment(self.command, url.query, body, self.headers.get("Content-Type", ""))
            result = subprocess.run([str(document / name)], input=body, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, env=env, timeout=5)
            head, data = result.stdout.split(b"\r\n\r\n", 1)
            headers = dict(line.split(": ", 1) for line in head.decode().split("\r\n"))
            status = int(headers.pop("Status", "200 OK").split()[0])
            self.send_response(status)
            for key, value in headers.items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(data)
        do_GET = serve
        do_POST = serve

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = "/data/mnt/sd_0/album"
    def request(name, fields, data=None):
        method, content_type = "GET", ""
        query = urlencode(fields)
        body = b""
        if name in {"create", "move", "delete"}:
            method, content_type = "POST", "application/x-www-form-urlencoded"
            body, query = query.encode(), ""
        if name == "upload":
            method, content_type, body = "POST", "application/octet-stream", data
        conn = http.client.HTTPConnection(*server.server_address, timeout=6)
        conn.request(method, "/" + name + ("?" + query if query else ""), body,
                     {"Content-Type": content_type})
        response = conn.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        conn.close()
        return result
    try:
        status, headers, data = request("sonix-list", {"path": base, "hidden": 1})
        assert status == 200 and headers["X-Sonix-Card"] == epoch
        assert [item["name"] for item in json.loads(data)] == ["ordinary.flac"]
        status, _, data = request("download", {"path": base + "/ordinary.flac", "epoch": epoch})
        assert status == 200 and data == ordinary
        for name in ("download", "upload", "delete"):
            status, _, _ = request(name, {"path": base + "/pending.flac", "epoch": epoch}, b"overwrite")
            assert status == 409
        assert (album / "pending.flac").read_bytes() == b"partial"
        for target in (base, "/data/mnt/sd_0"):
            assert request("delete", {"path": target, "epoch": epoch})[0] == 409
        assert request("move", {"oldPath": base, "newPath": "/data/mnt/sd_0/moved", "epoch": epoch})[0] == 409
        for path in (base + "/.sonix-download-publication.db", base + "/.nd-1-0123456789abcdef.flac"):
            assert request("upload", {"path": path, "epoch": epoch}, b"bad")[0] == 409
        assert request("upload", {"path": base + "/unrelated.bin", "epoch": epoch}, ordinary)[0] == 200
        assert (album / "unrelated.bin").read_bytes() == ordinary
        assert request("create", {"path": base + "/new", "epoch": epoch})[0] == 200
        assert request("move", {"oldPath": base + "/unrelated.bin", "newPath": base + "/new/moved.bin", "epoch": epoch})[0] == 200
        assert request("delete", {"path": base + "/new", "epoch": epoch})[0] == 200
        print("PASS loopback guarded Wi-Fi: partials allow enable and unrelated operations; protected files and ancestors reject mutations")
        outside = root / "outside"
        outside.mkdir()
        (outside / "secret").write_bytes(b"outside")
        (album / "alias").symlink_to(outside, target_is_directory=True)
        for path in (base + "/alias/secret", base + "/../album/ordinary.flac", base + "/.sonix-download-publication.db"):
            assert request("download", {"path": path, "epoch": epoch})[0] == 409
        assert request("download", {"path": base + "/ordinary.flac"})[0] == 409
        for name in ("download.cgi", "upload.cgi", "pages/index.html", "sd_0/album/pending.flac", "../download"):
            assert request(name, {"path": base + "/pending.flac", "epoch": epoch})[0] == 404
        assert request("list", {"path": base, "hidden": 1})[0] == 200
        print("PASS isolated document root and direct CGI requests: no stock/static bypass, path escape or missing card identity")

        # Leave a real upload process waiting for more transport bytes. Card
        # detach/replace must finish without waiting for that browser.
        query = urlencode({"path": base + "/slow.bin", "epoch": epoch})
        env = environment("POST", query, b"", "application/octet-stream")
        env["CONTENT_LENGTH"] = str(2 * 65536)
        proc = subprocess.Popen([str(document / "upload")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        proc.stdin.write(b"x" * 65536)
        proc.stdin.flush()
        for _ in range(100):
            if (album / "slow.bin").exists() and (album / "slow.bin").stat().st_size == 65536:
                break
            time.sleep(.01)
        assert (album / "slow.bin").stat().st_size == 65536
        replacement = root / "replacement"
        (replacement / "album").mkdir(parents=True)
        (replacement / "album/slow.bin").write_bytes(b"replacement preserved")
        new_epoch = control("root", replacement)
        assert new_epoch != epoch
        proc.stdin.write(b"y" * 65536)
        proc.stdin.close()
        output = proc.stdout.read()
        assert proc.wait(timeout=5) == 1 and b"Status: 409" in output
        assert (replacement / "album/slow.bin").read_bytes() == b"replacement preserved"
        assert request("upload", {"path": base + "/stale.bin", "epoch": epoch}, b"bad")[0] == 409
        assert not (replacement / "album/stale.bin").exists()
        print("PASS real CGI transport stall: card replacement drains, old upload and stale page cannot write replacement")
        control("disable")
        assert request("sonix-list", {"path": base})[0] == 409
        control("enable")
        assert request("sonix-list", {"path": base})[0] == 200
        print("PASS Wi-Fi disable invalidates request admission independently of the selected card")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
        control("cleanup", document)
