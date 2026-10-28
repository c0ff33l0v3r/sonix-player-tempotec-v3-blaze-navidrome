#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-playlist-check.XXXXXX)
check_sqlite_temp=$(mktemp /tmp/sonix-playlist-sqlite.XXXXXX)
trap 'rm -f "$check_binary" "$check_sqlite_temp"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    # Vendor warnings do not weaken -Werror for the application/fixture sources.
    ${CC:-cc} -std=gnu11 -O1 -g ${NAVIDROME_CHECK_FLAGS:-} -w -I. \
        -c src/system/db/sqlite3_impl.c -o "$check_sqlite_temp"
    check_sqlite=$check_sqlite_temp
fi
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g ${NAVIDROME_CHECK_FLAGS:-} \
 -ffunction-sections -fdata-sections -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
 tools/navidrome_playlistcheck.c tools/navidrome_test_support.c \
 src/system/streaming/navidromeplaycatalog.c src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c \
 src/system/streaming/navidromedebug.c src/system/streaming/navidromeapi.c src/system/streaming/navidromestore.c src/system/streaming/navidromecrypto.c \
 src/system/core/json.c src/system/core/md5.c src/system/core/sha256.c src/system/device/workactivity.c src/system/net/http.c src/system/net/tls.c src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c \
 "$check_sqlite" -Wl,--gc-sections,--wrap=http_finite_request,--wrap=http_request,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free -lm -pthread -ldl -lresolv -o "$check_binary"
python3 tools/navidrome_playlistcheck.py "$check_binary"
