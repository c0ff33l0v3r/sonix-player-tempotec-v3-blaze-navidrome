#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-navidrome-discoverycheck.XXXXXX)
check_binary="$check_dir/check"
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    check_sqlite="$check_dir/sqlite.o"
    ${CC:-cc} -std=gnu11 -O1 -DHOST_BUILD -I. -c src/system/db/sqlite3_impl.c -o "$check_sqlite"
fi
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_discoverycheck.c src/system/streaming/navidromediscovery.c src/system/library/library.c \
    src/system/streaming/navidromesearch.c src/system/streaming/navidromemembership.c src/system/streaming/navidromedebug.c \
    src/system/streaming/navidromeplaycatalog.c src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c src/system/device/workactivity.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c src/system/streaming/navidromestore.c \
    src/system/streaming/navidromelibrary.c src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/core/json.c src/system/core/md5.c src/system/net/http.c src/system/net/tls.c \
    "$check_sqlite" -Wl,--gc-sections,--wrap=nd_pl_append -lm -pthread -ldl -lresolv -o "$check_binary"
python3 tools/navidrome_discoverycheck.py "$check_binary"
