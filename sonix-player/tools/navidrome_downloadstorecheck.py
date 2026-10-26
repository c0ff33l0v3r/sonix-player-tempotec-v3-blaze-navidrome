#!/usr/bin/env python3
"""Focused durable-download-store checks, without building the player."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

player = Path(__file__).resolve().parent.parent
compiler = shlex.split(os.environ.get("CC", "cc"))
extra = shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", ""))
with tempfile.TemporaryDirectory(prefix="sonix-download-store-") as temporary:
    root = Path(temporary)
    sqlite = player / "build_host/system/db/sqlite3_impl.o"
    if not sqlite.exists():
        sqlite = root / "sqlite.o"
        subprocess.run(compiler + ["-std=gnu11", "-w", "-I", str(player), "-c", str(player / "src/system/db/sqlite3_impl.c"), "-o", str(sqlite)] + extra, check=True)
    binary = root / "storecheck"
    subprocess.run(compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-D_FILE_OFFSET_BITS=64", "-I", str(player)] + extra + [str(player / "tools/navidrome_downloadstorecheck.c"), str(player / "src/system/streaming/navidromedownloadstore.c"), str(player / "src/system/core/sha256.c"), str(sqlite), "-pthread", "-ldl", "-lm", "-o", str(binary)], check=True)
    card = root / "card"
    card.mkdir()
    subprocess.run([str(binary), str(card)], check=True, timeout=60)
