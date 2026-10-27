#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_sqlite=src/system/db/sqlite3_impl.c
if [ -f build_host/system/db/sqlite3_impl.o ]; then
    check_sqlite=build_host/system/db/sqlite3_impl.o
fi
check_binary=$(mktemp /tmp/sonix-navidrome-favoritescheck.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_favoritescheck.c tools/navidrome_test_support.c tools/navidrome_cover_test_support.c \
    src/gui/streaming/navidromeart.c src/system/image/stb_image_impl.c \
    src/system/streaming/navidromedebug.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c \
    src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c src/system/streaming/navidromestars.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/core/json.c src/system/core/md5.c src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/net/http.c src/system/net/tls.c \
    "$check_sqlite" -lm -pthread -ldl -lresolv -o "$check_binary"
python3 tools/navidrome_favoritescheck.py "$check_binary"
