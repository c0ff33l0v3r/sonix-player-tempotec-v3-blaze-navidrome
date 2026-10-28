"""Real loopback report requests; synthetic credentials/card/audio only."""
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from urllib.parse import parse_qs, urlsplit

seen = {}
errors = []
lock = threading.Lock()


class Fixture(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        try:
            self.handle_report()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as exc:
            with lock:
                errors.append(type(exc).__name__)
            self.close_connection = True

    def handle_report(self):
        url = urlsplit(self.path)
        route = url.path.split("/")[1]
        method = url.path.rsplit("/", 1)[1].removesuffix(".view")
        query = parse_qs(url.query)
        assert query["u"] == ["fixture user"]
        assert query["v"] == ["1.16.1"] and query["c"] == ["SonixPlayer"]
        token = hashlib.md5(("synthetic-server-password" + query["s"][0]).encode()).hexdigest()
        assert query["t"] == [token]
        with lock:
            seen.setdefault(route, []).append((method, query))
        if route == "preempt" and method != "getOpenSubsonicExtensions":
            time.sleep(3)  # report socket can be cancelled while new audio starts
        response = {"subsonic-response": {"status": "ok"}}
        status = 200
        if method == "getOpenSubsonicExtensions":
            response["subsonic-response"]["openSubsonicExtensions"] = (
                [] if route == "legacy" else [{"name": "playbackReport", "versions": [1]}]
            )
        elif route in ("auth", "auth-auto"):
            status = 401
            response = {"subsonic-response": {"status": "failed", "error": {"code": 40}}}
        elif route == "lost" and query.get("submission") == ["true"]:
            self.close_connection = True
            return  # fixture received/applied this event; acknowledgment lost
        elif route == "redirect":
            status = 302
        body = json.dumps(response).encode()
        if route == "malformed" and method != "getOpenSubsonicExtensions":
            body = b'{"subsonic-response":'
        if route == "oversize" and method != "getOpenSubsonicExtensions":
            body += b" " * 65536
        self.send_response(status)
        if route == "redirect" and method != "getOpenSubsonicExtensions":
            self.send_header("Location", "/must-not-replay/rest/scrobble.view?" + url.query)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Private-Canary", "RESPONSE_HEADER_CANARY")
        self.end_headers()
        self.wfile.write(body)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server:
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        cases = ["modern", "legacy", "auth", "malformed", "oversize", "redirect",
                 "dns", "lost", "priority", "store-stall", "preempt", "overflow", "idle", "init-fail", "offline", "lifecycle", "undated", "auto-only", "auth-auto", "external-release"]
        if os.environ.get("SONIX_REPORT_CASES"):
            cases = os.environ["SONIX_REPORT_CASES"].split(",")
        for route in cases:
            url = f"http://127.0.0.1:{server.server_port}/{route}/base"
            result = subprocess.run([binary, url, route], capture_output=True, text=True, timeout=45)
            output = result.stdout + result.stderr
            if result.returncode:
                # Sanitized fixtures never print request URLs/credentials.
                print(output[-6000:])
                raise AssertionError(f"report worker {route}: exit {result.returncode}")
            for secret in ("synthetic-server-password", "fixture user", "?u=", "&t=", "RESPONSE_HEADER_CANARY"):
                assert secret not in output, f"redaction failed: {route}"
            with lock:
                requests = list(seen.get(route, []))
            if route in ("idle", "init-fail", "auto-only"):
                assert not requests, (route, len(requests))
                print(f"report {route}: silent and zero network work: passed", flush=True)
                continue
            assert requests, f"no real HTTP exercised for {route}"
            if route == "dns":
                assert len(requests) == 1 and requests[0][0] == "probe"
                print("report dedicated resolver isolation/cancellation: passed", flush=True)
                continue
            discoveries = sum(m == "getOpenSubsonicExtensions" for m, _ in requests)
            assert discoveries == (2 if route == "lost" else 1), (route, discoveries)
            counted = [q for m, q in requests if m == "scrobble" and q.get("submission") == ["true"]]
            if route == "overflow":
                assert 1 <= len(counted) <= 5
                assert len({q["time"][0] for q in counted}) == len(counted)
            elif route in ("auth", "auth-auto", "malformed", "oversize", "redirect", "preempt"):
                assert len(counted) <= 1
            else:
                assert len(counted) == 1, (route, len(counted))
            for q in counted:
                assert q["id"] == ["song-00000"]
                assert q["time"] == ["1700000100000"] or route == "overflow"
            modern = [q for m, q in requests if m == "reportPlayback"]
            if route == "legacy":
                assert not modern
                assert any(q.get("submission") == ["false"] for m, q in requests if m == "scrobble")
            for q in modern:
                assert q["ignoreScrobble"] == ["true"] and q["mediaType"] == ["song"]
                assert int(q["positionMs"][0]) >= 0
            assert not errors, errors
            print(f"report loopback/coordinator {route}: passed ({len(requests)} bounded requests)", flush=True)
        assert "must-not-replay" not in seen
        server.shutdown()


if __name__ == "__main__":
    main()
