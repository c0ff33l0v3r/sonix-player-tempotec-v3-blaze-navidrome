#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-publication-check.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
check_sqlite=src/system/db/sqlite3_impl.c
if [ -f build_host/system/db/sqlite3_impl.o ]; then
    check_sqlite=build_host/system/db/sqlite3_impl.o
fi
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -ffunction-sections -fdata-sections \
    ${NAVIDROME_CHECK_FLAGS:-} -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_publicationcheck.c src/system/device/workactivity.c \
    src/system/streaming/navidromestore.c src/system/streaming/navidromecrypto.c "$check_sqlite" \
    -Wl,--gc-sections -pthread -ldl -lm -o "$check_binary"
timeout 15 "$check_binary"
