#!/usr/bin/env python3
"""Native journal/admission checks; never mounts or modifies a real card."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

player = Path(__file__).resolve().parent.parent
cc = shlex.split(os.environ.get("CC", "cc"))
flags = shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", ""))
with tempfile.TemporaryDirectory(prefix="sonix-file-publication-") as tmp:
    root = Path(tmp)
    sqlite = root / "sqlite.o"
    subprocess.run(cc + ["-std=gnu11", "-O2", "-w", "-I", str(player), "-c",
                         str(player / "src/system/db/sqlite3_impl.c"),
                         "-o", str(sqlite)] + flags, check=True)
    binary = root / "guardcheck"
    subprocess.run(cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                         "-D_FILE_OFFSET_BITS=64", "-DFILEPUBLICATION_TEST", "-I", str(player)] + flags + [
        str(player / "tools/filepublicationcheck.c"),
        str(player / "src/system/library/filepublication.c"), str(sqlite),
        str(player / "src/system/streaming/navidromedownloadcopy.c"),
        str(player / "src/system/streaming/navidromedownloadfile.c"),
        str(player / "src/system/core/sha256.c"),
        "-Wl,--wrap=write,--wrap=fsync,--wrap=close", "-pthread", "-ldl", "-lm", "-o", str(binary)], check=True)
    card = root / "card"
    card.mkdir()
    subprocess.run([str(binary), str(card)], check=True, timeout=60)
