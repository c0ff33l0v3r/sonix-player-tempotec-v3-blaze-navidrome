#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-playlist-sortcheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    ${CC:-cc} -std=gnu11 -O1 -w -I. -c src/system/db/sqlite3_impl.c -o "$check_dir/sqlite.o"
    check_sqlite=$check_dir/sqlite.o
fi
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections \
    ${NAVIDROME_CHECK_FLAGS:-} -I. tools/navidrome_playlistsortcheck.c \
    src/system/streaming/navidromeplayliststore.c src/system/library/library.c \
    src/system/core/sha256.c src/system/core/json.c "$check_sqlite" \
    -pthread -ldl -lm -Wl,--gc-sections,--wrap=sqlite3_step,--wrap=calloc,--wrap=sqlite3_exec -o "$check_dir/check"
"$check_dir/check" write "$check_dir"
"$check_dir/check" read "$check_dir"
