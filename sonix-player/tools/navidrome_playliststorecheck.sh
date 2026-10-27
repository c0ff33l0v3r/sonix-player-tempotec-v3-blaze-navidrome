#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-playlist-storecheck.XXXXXX)
check_sqlite_temp=$(mktemp /tmp/sonix-playlist-sqlite.XXXXXX)
trap 'rm -f "$check_binary" "$check_sqlite_temp"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    # Vendor warnings do not weaken -Werror for the application/fixture sources.
    ${CC:-cc} -std=gnu11 -O1 -g ${NAVIDROME_CHECK_FLAGS:-} -w -I. \
        -c src/system/db/sqlite3_impl.c -o "$check_sqlite_temp"
    check_sqlite=$check_sqlite_temp
fi
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} -I. \
 tools/navidrome_playliststorecheck.c src/system/streaming/navidromeplayliststore.c src/system/library/library.c \
 src/system/core/json.c src/system/core/sha256.c "$check_sqlite" -pthread -ldl -lm -Wl,--gc-sections -o "$check_binary"
"$check_binary"
