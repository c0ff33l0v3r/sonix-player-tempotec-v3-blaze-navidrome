#!/usr/bin/env python3
"""Production file-manager primitives and native WAV; no display/audio device."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

player = Path(__file__).resolve().parent.parent
lvgl = Path(os.environ.get("SONIX_LVGL_ROOT", player / "lvgl"))
if not (lvgl / "lvgl.h").is_file():
    raise SystemExit("Requires LVGL headers; set SONIX_LVGL_ROOT. No check ran.")
cc = shlex.split(os.environ.get("CC", "cc"))
extra = shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", ""))
with tempfile.TemporaryDirectory(prefix="sonix-publication-consumers-") as tmp:
    root = Path(tmp)
    sqlite = root / "sqlite.o"
    subprocess.run(cc + ["-std=gnu11", "-O2", "-w", "-I", str(player), "-c",
                         str(player / "src/system/db/sqlite3_impl.c"), "-o", str(sqlite)] + extra, check=True)
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "alsa", "freetype2", "sdl2"], text=True))
    for fixture in ("filepublicationconsumercheck", "filepublicationwavcheck"):
        binary = root / fixture
        subprocess.run(cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            "-Wno-missing-field-initializers", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                            "-DHOST_BUILD", "-D_FILE_OFFSET_BITS=64", "-DLV_CONF_INCLUDE_SIMPLE", "-I", str(player),
                            "-I", str(lvgl.parent), "-I", str(lvgl)] + flags + extra + [
            str(player / "tools" / (fixture + ".c")), str(player / "src/system/library/filepublication.c"),
            str(player / "src/system/library/filepublicationcard.c"),
            str(player / "src/system/device/workactivity.c"), str(sqlite), "-Wl,--gc-sections", "-pthread", "-ldl", "-lm", "-lasound", "-o", str(binary)], check=True)
        card = root / (fixture + "-card")
        card.mkdir()
        subprocess.run([str(binary), str(card)], check=True, timeout=30)
