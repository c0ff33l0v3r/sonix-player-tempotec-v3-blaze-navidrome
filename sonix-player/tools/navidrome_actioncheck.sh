#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-navidrome-actions.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    check_sqlite="$check_dir/sqlite.o"
    ${CC:-cc} -std=gnu11 -O1 -DHOST_BUILD -I. -c src/system/db/sqlite3_impl.c -o "$check_sqlite"
fi
for mode in normal limit; do
check_limit=
if [ "$mode" = limit ]; then check_limit=-DND_ACTION_PAGES=8; fi
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} $check_limit \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_actioncheck.c src/system/library/library.c src/system/streaming/navidromeactions.c \
    src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/streaming/navidromestars.c src/system/streaming/navidromesearch.c src/system/streaming/navidromemembership.c src/system/streaming/navidromedebug.c \
    src/system/streaming/navidromeplaycatalog.c src/system/device/workactivity.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c src/system/streaming/navidromestore.c \
    src/system/streaming/navidromelibrary.c src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/core/json.c src/system/core/md5.c src/system/net/http.c src/system/net/tls.c \
    "$check_sqlite" -Wl,--gc-sections,--wrap=calloc,--wrap=sqlite3_step -lm -pthread -ldl -lresolv -o "$check_dir/check"
if [ "$mode" = limit ]; then python3 tools/navidrome_actioncheck.py "$check_dir/check" limit; else python3 tools/navidrome_actioncheck.py "$check_dir/check"; fi
done
