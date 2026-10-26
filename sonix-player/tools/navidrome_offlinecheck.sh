#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-navidrome-offline.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    ${CC:-cc} -std=gnu11 -w -I. -c src/system/db/sqlite3_impl.c -o "$check_dir/sqlite.o"
    check_sqlite=$check_dir/sqlite.o
fi
python3 - "$check_dir" <<'PY'
import sys,wave
from pathlib import Path
root=Path(sys.argv[1]); (root/'card').mkdir()
with wave.open(str(root/'original.wav'),'wb') as out:
    out.setnchannels(1); out.setsampwidth(2); out.setframerate(44100); out.writeframes(b'\0\0'*44100)
PY
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -D_FILE_OFFSET_BITS=64 -DNAVIDROME_DOWNLOAD_PUBLISH_TEST \
    ${NAVIDROME_CHECK_FLAGS:-} -I. -Ilvgl \
    tools/navidrome_offlinecheck.c tools/navidrome_test_support.c \
    src/system/streaming/navidromedownloads.c src/system/streaming/navidromedownloadstore.c \
    src/system/streaming/navidromedownloadfile.c src/system/streaming/navidromeaudio.c \
    src/system/streaming/navidromestore.c src/system/streaming/navidromeplayliststore.c \
    src/system/streaming/streamturn.c src/system/streaming/navidromecrypto.c src/system/core/sha256.c "$check_sqlite" \
    -Wl,--gc-sections,--wrap=write,--wrap=fsync,--wrap=fstatvfs64,--wrap=nd_pl_play_window \
    -pthread -ldl -lm -o "$check_dir/check"
"$check_dir/check" "$check_dir/card" "$check_dir/original.wav"
