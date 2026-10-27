#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-playlist-playcheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    ${CC:-cc} -std=gnu11 -O1 -g ${NAVIDROME_CHECK_FLAGS:-} -w -I. -c src/system/db/sqlite3_impl.c -o "$check_dir/sqlite.o"
    check_sqlite=$check_dir/sqlite.o
fi
for mode in normal limit; do
    check_limit=
    if [ "$mode" = limit ]; then check_limit=-DND_PLAYCATALOG_PAGES=8; fi
    ${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections \
        ${NAVIDROME_CHECK_FLAGS:-} $check_limit -I. tools/navidrome_playlistplaycheck.c \
        tools/navidrome_test_support.c src/system/streaming/navidromeplayliststore.c \
        src/system/core/sha256.c src/system/core/json.c "$check_sqlite" \
        -pthread -ldl -lm -Wl,--gc-sections,--wrap=sqlite3_step,--wrap=calloc -o "$check_dir/$mode"
    if [ "$mode" = limit ]; then "$check_dir/$mode" limit; else "$check_dir/$mode"; fi
done
