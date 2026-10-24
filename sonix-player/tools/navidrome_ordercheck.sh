#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-navidrome-ordercheck.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
check_sqlite=src/system/db/sqlite3_impl.c
if [ -f build_host/system/db/sqlite3_impl.o ]; then
    check_sqlite=build_host/system/db/sqlite3_impl.o
fi
# Compile the real shared ordering implementation; unused local-library code is
# discarded, so this focused check needs neither a full build nor SQLite stubs.
${CC:-cc} -std=gnu11 -Wall -O1 -ffunction-sections -fdata-sections \
    -DHOST_BUILD -D_FILE_OFFSET_BITS=64 -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_ordercheck.c src/system/library/library.c src/system/streaming/navidromestore.c \
    "$check_sqlite" -Wl,--gc-sections -pthread -ldl -lm -o "$check_binary"
"$check_binary"
