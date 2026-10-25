#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires LVGL objects from an existing host build; no check ran.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-coordinatorcheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
find build_host/lvgl -name '*.o' -print > "$check_dir/objects"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -Ilvgl \
    tools/navidrome_coordinatorcheck.c tools/navidrome_test_support.c lvgl/src/font/lv_font_unscii_8.c \
    src/system/streaming/navidromeplay.c src/system/playback/playlist.c src/system/decode/growfile.c \
    src/system/streaming/streamturn.c src/system/streaming/navidromedebug.c \
    src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c \
    src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/streaming/navidromestars.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/core/json.c src/system/core/md5.c build_host/system/db/sqlite3_impl.o \
    src/system/streaming/navidromebatch.c src/system/net/http.c src/system/net/tls.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c \
    @"$check_dir/objects" -pthread -ldl -lm -lresolv $(pkg-config --libs freetype2 sdl2) \
    -Wl,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open \
    -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink,--wrap=malloc -o "$check_dir/check"
"$check_dir/check"
