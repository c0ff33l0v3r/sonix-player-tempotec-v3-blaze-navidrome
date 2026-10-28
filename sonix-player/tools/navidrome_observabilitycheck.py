#!/usr/bin/env python3
"""Production-path emission census; real writer is tested separately by debugcheck.
No full builds, device access or real account data. Requires existing LVGL objects.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

PLAYER = Path(__file__).resolve().parents[1]
CASES = {
    "service": ("navidrome_selftest.sh", 0),
    "cache": ("navidrome_cachecheck.py", 20),
    "mailbox": ("navidrome_noticecheck.sh", 18),
    "vault": ("navidrome_selftest.sh", 21),
    "sync": ("navidrome_synccheck.sh", 1),
    "commit": ("navidrome_publicationcheck.sh", 2),
    "playback": ("navidrome_playlistcoordinatorcheck.sh", 3),
    "decoder": ("navidrome_audiocheck.sh", 4),
    "playerart": ("navidrome_playerartuicheck.sh", 5),
    "artwork": ("navidrome_artworkcheck.sh", 5),
    "browse": ("navidrome_browsecheck.sh", 6),
    "playlist": ("navidrome_playlistcheck.sh", 7),
    "search": ("navidrome_searchuicheck.sh", 8),
    "discovery": ("navidrome_discoveryuicheck.sh", 9),
    "download": ("navidrome_downloadcheck.py", 10),
    "offline": ("navidrome_offlinecheck.sh", 11),
    "favorites": ("navidrome_favoritescheck.sh", 12),
    "actions": ("navidrome_actioncheck.sh", 13),
    "lyrics": ("navidrome_lyricscheck.sh", 14),
    "reports": ("navidrome_reportcheck.sh", 15),
    "session": ("navidrome_sessioncheck.sh", 16),
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--only", nargs="+", choices=CASES)
parser.add_argument("--output", type=Path, help="Keep numeric emission evidence (no fixture payloads).")
args = parser.parse_args()
selected = args.only or list(CASES)
evidence = {}
failed = False
with tempfile.TemporaryDirectory(prefix="sonix-observability-") as temp:
    root = Path(temp)
    recorder = root / "recorder.o"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(PLAYER), "-c", str(PLAYER / "tools/navidrome_trace_fixture.c"), "-o", str(recorder)], check=True)
    for name in selected:
        script, component = CASES[name]
        report = root / (name + ".jsonl")
        env = dict(os.environ, SONIX_TRACE_REPORT=str(report))
        env["NAVIDROME_CHECK_FLAGS"] = env.get("NAVIDROME_CHECK_FLAGS", "") + " " + str(recorder)
        if name == "browse":
            env["NAVIDROME_CHECK_FLAGS"] += " -DNAVIDROME_TRACE_QUERY_DELAY"
        command = ["python3" if script.endswith(".py") else "sh", str(PLAYER / "tools" / script)]
        log = root / (name + ".txt")
        print(f"Checking {name}: production behavior and emitted events", flush=True)
        with log.open("w") as output:
            result = subprocess.run(command, cwd=PLAYER.parent, env=env, stdout=output, stderr=subprocess.STDOUT)
        rows = [json.loads(line) for line in report.read_text().splitlines()] if report.exists() else []
        relevant = [row for row in rows if row["component"] == component]
        success = sum(row["count"] for row in relevant if row["outcome"] == 1)
        rejected = sum(row["count"] for row in relevant if row["outcome"] in (2, 3, 4, 6, 7))
        passed = result.returncode == 0 and success > 0 and rejected > 0
        if name == "browse":
            passed = passed and any(row["event"] == 36 and row["duration_max_us"] >= 25000 for row in relevant)
        if name == "artwork":
            passed = passed and any(row["event"] == 21 for row in relevant)
        if name == "mailbox":
            passed = passed and any(row["event"] == 15 and row["duration_max_us"] >= 25000 for row in relevant)
        if name == "commit":
            passed = passed and any(row["event"] == 11 and row["duration_max_us"] >= 25000 for row in relevant)
        if name == "decoder":
            passed = passed and any(row["event"] == 23 and row["outcome"] == 2 and row.get("accepted_parent_decode_failures", 0) > 0 for row in relevant) and any(row["event"] == 7 and row["outcome"] == 1 for row in relevant)
        if name == "playerart":
            passed = passed and any(row["event"] == 37 and row["outcome"] == 1 for row in relevant) and any(row["event"] == 37 and row["outcome"] == 6 for row in relevant)
        evidence[name] = {"exit": result.returncode, "success_events": success, "rejected_events": rejected, "events": rows, "passed": passed}
        if not passed:
            failed = True
            print(log.read_text()[-6000:])
        print(f"{name}: {'PASS' if passed else 'FAIL'}; successful={success}, rejected/retired/uncertain={rejected}", flush=True)
if args.output:
    args.output.write_text(json.dumps(evidence, indent=2) + "\n")
raise SystemExit(1 if failed else 0)
