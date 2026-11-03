#!/usr/bin/env python3
"""Cross-process storage lifetime checks; private control files, no mounting."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

player = Path(__file__).resolve().parent.parent
cc = shlex.split(os.environ.get("CC", "cc"))
extra = shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", ""))
with tempfile.TemporaryDirectory(prefix="sonix-card-fence-") as tmp:
    root = Path(tmp)
    sqlite = root / "sqlite.o"
    subprocess.run(cc + ["-std=gnu11", "-O2", "-w", "-I", str(player), "-c",
                         str(player / "src/system/db/sqlite3_impl.c"), "-o", str(sqlite)] + extra, check=True)
    binary = root / "cardcheck"
    subprocess.run(cc + ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-I", str(player), '-DFP_CARD_CONTROL="' + str(root / "control") + '"'] + extra + [
        str(player / "tools/filepublicationcardcheck.c"),
        str(player / "src/system/library/filepublicationcard.c"),
        str(player / "src/system/library/filepublication.c"), str(sqlite),
        "-pthread", "-ldl", "-lm", "-o", str(binary)], check=True)
    card = root / "card"
    card.mkdir()
    subprocess.run([str(binary), str(card)], check=True, timeout=30)
