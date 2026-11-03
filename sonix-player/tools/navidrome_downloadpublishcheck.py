#!/usr/bin/env python3
"""Compile only the publication primitive and exercise it in a synthetic tree."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

player = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="sonix-download-publish-") as temporary:
    root = Path(temporary)
    binary = root / "publishcheck"
    files = root / "files"
    files.mkdir()
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-D_FILE_OFFSET_BITS=64", "-DNAVIDROME_DOWNLOAD_PUBLISH_TEST",
        "-I", str(player),
    ]
    command += shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", ""))
    command += [
        str(player / "tools/navidrome_downloadpublishcheck.c"),
        str(player / "src/system/streaming/navidromedownloadfile.c"),
        "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary), str(files)], check=True, timeout=30)
    absent = root / "nosyscheck"
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_FILE_OFFSET_BITS=64",
        "-DNAVIDROME_DOWNLOAD_NO_RENAMEAT2", "-I", str(player),
    ] + shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", "")) + [
        str(player / "tools/navidrome_downloadnosyscheck.c"),
        str(player / "src/system/streaming/navidromedownloadfile.c"),
        "-o", str(absent),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(absent), str(files)], check=True, timeout=30)
