#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-lyrics.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -O1 -w ${NAVIDROME_CHECK_FLAGS:-} -DHOST_BUILD -I. -c src/system/db/sqlite3_impl.c -o "$check_dir/sqlite.o"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} -DLRCLIB_TEST -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -I"${SONIX_LVGL_SOURCE:-lvgl}/.." -I"${SONIX_LVGL_SOURCE:-lvgl}" \
    tools/navidrome_lyricscheck.c src/system/library/lyrics.c src/system/library/lrclib.c \
    src/system/streaming/navidromelyricparse.c src/system/streaming/navidromelyricstore.c \
    src/system/streaming/navidromelyrics.c src/system/streaming/navidromeapi.c \
    src/system/streaming/navidromecrypto.c src/system/core/json.c src/system/core/md5.c \
    "$check_dir/sqlite.o" src/system/device/workactivity.c src/system/net/http.c src/system/net/tls.c src/system/net/gzip.c \
    src/gb/miniz/miniz.c src/system/image/png_scaled.c -Wl,--gc-sections,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=sqlite3_exec,--wrap=getaddrinfo -pthread -lm -ldl -lresolv -o "$check_dir/check"
python3 tools/navidrome_lyricscheck.py "$check_dir/check"
