#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# Reuse the bundled SQLite object when available; otherwise compile only its wrapper.
check_sqlite=src/system/db/sqlite3_impl.c
if [ -f build_host/system/db/sqlite3_impl.o ]; then
    check_sqlite=build_host/system/db/sqlite3_impl.o
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-cleanupcheck.XXXXXX)
check_binary="$check_dir/check"
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_cleanupcheck.c src/system/streaming/navidromedebug.c src/system/device/workactivity.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c tools/navidrome_test_support.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/net/http.c src/system/net/tls.c src/system/core/json.c src/system/core/md5.c "$check_sqlite" -lm -pthread -ldl \
    -lresolv -Wl,--wrap=http_finite_request,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open -Wl,--wrap=rename,--wrap=fsync,--wrap=unlink,--wrap=nd_cache_retire -o "$check_binary"
mkdir "$check_dir/before" "$check_dir/after"
"$check_binary" before "$check_dir/before"
"$check_binary" after "$check_dir/after"
