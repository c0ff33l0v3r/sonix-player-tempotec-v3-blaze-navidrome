#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_objects=${SONIX_LVGL_OBJECTS:-build_host/lvgl}
check_source=${SONIX_LVGL_SOURCE:-lvgl}
if [ ! -f "$check_objects/src/lv_init.o" ]; then
    echo 'Requires LVGL objects from an existing host build.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-star-uicheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=${SONIX_SQLITE_OBJECT:-src/system/db/sqlite3_impl.c}
if [ -z "${SONIX_SQLITE_OBJECT:-}" ] && [ -f build_host/system/db/sqlite3_impl.o ]; then
    check_sqlite=build_host/system/db/sqlite3_impl.o
fi
find "$check_objects" -name '*.o' -print > "$check_dir/lvgl-objects"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -I"$check_source/.." -I"$check_source" \
    tools/navidrome_star_uicheck.c src/gui/streaming/navidromestar.c tools/navidrome_test_support.c "$check_source/src/font/lv_font_unscii_8.c" \
    src/system/streaming/navidromedebug.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c \
    src/system/streaming/navidromeplaycatalog.c src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/streaming/navidromestars.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/net/http.c src/system/net/tls.c src/system/core/json.c src/system/core/md5.c "$check_sqlite" \
    @"$check_dir/lvgl-objects" -pthread -ldl -lm $(pkg-config --libs freetype2 sdl2) \
    -Wl,--wrap=nd_api_finite_json,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink -o "$check_dir/check"
"$check_dir/check"
"$check_dir/check" 480 800
