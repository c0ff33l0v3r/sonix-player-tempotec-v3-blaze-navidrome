#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# Reuse the bundled SQLite object when available; otherwise compile only its wrapper.
check_sqlite=src/system/db/sqlite3_impl.c
if [ -f build_host/system/db/sqlite3_impl.o ]; then
    check_sqlite=build_host/system/db/sqlite3_impl.o
fi
# Reuse LVGL objects from the user's host build; never run a full player build.
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires LVGL objects from an existing host build.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-uicheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
find build_host/lvgl -name '*.o' -print > "$check_dir/lvgl-objects"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    -DLV_FONT_UNSCII_8=1 tools/navidrome_uicheck.c lvgl/src/font/lv_font_unscii_8.c \
    src/system/streaming/navidromecache.c src/system/streaming/navidromedebug.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c tools/navidrome_test_support.c src/system/streaming/navidromecrypto.c \
    src/system/streaming/navidromevault.c src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/net/http.c src/system/net/tls.c src/system/core/json.c src/system/core/md5.c \
    @"$check_dir/lvgl-objects" "$check_sqlite" -lm -pthread -ldl -lm $(pkg-config --libs freetype2 sdl2) \
    -Wl,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink,--wrap=nd_cache_usage_snapshot,--wrap=navidrome_library_snapshot -o "$check_dir/check"
"$check_dir/check"
