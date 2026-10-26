#!/bin/sh
set -eu
sh "$(dirname "$0")/navidrome_reportaudiocheck.sh"
cd "$(dirname "$0")/.."
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires LVGL objects from an existing host build; no check ran.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-coordinatorcheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
polish_sources=
polish_flags=
if [ "${NAVIDROME_POLISH_QUEUE_UI:-0}" = 1 ]; then
    polish_sources='tools/navidrome_queue_uifixture.c tools/navidrome_cover_test_support.c src/gui/nowplaying/coverloader.c src/gui/streaming/navidromeart.c src/system/image/stb_image_impl.c'
    polish_flags='-DNAVIDROME_POLISH_QUEUE_UI -Wno-missing-field-initializers'
fi
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_reportcheck.c src/system/streaming/navidromereporttrack.c \
    src/system/streaming/navidromereportstore.c src/system/lastfm/lastfm.c \
    build_host/system/db/sqlite3_impl.o -Wl,--gc-sections -pthread -ldl -lm -o "$check_dir/core"
"$check_dir/core"
find build_host/lvgl -name '*.o' -print > "$check_dir/objects"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -ffunction-sections -fdata-sections $polish_flags ${NAVIDROME_CHECK_FLAGS:-} \
    -DNAVIDROME_REPORT_TEST -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -Ilvgl \
    $polish_sources tools/navidrome_reportcoordinator.c tools/navidrome_test_support.c lvgl/src/font/lv_font_unscii_8.c \
    src/system/streaming/navidromereports.c src/system/streaming/navidromereportstore.c src/system/streaming/navidromereporttrack.c src/system/lastfm/lastfm.c src/system/streaming/navidromeplay.c src/system/playback/playlist.c src/system/decode/growfile.c \
    src/system/streaming/navidromecache.c src/system/streaming/streamturn.c src/system/streaming/navidromedebug.c \
    src/system/streaming/navidromeplaycatalog.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c \
    src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/streaming/navidromestars.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/core/json.c src/system/core/md5.c build_host/system/db/sqlite3_impl.o \
    src/system/streaming/navidromebatch.c src/system/net/http.c src/system/net/tls.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c \
    @"$check_dir/objects" -pthread -ldl -lm -lresolv $(pkg-config --libs freetype2 sdl2) \
    -Wl,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open \
    -Wl,--gc-sections,--wrap=pthread_create,--wrap=getaddrinfo,--wrap=sqlite3_exec,--wrap=rename,--wrap=fsync,--wrap=unlink,--wrap=malloc -o "$check_dir/check"
"$check_dir/check" --baseline
python3 tools/navidrome_reportcheck.py "$check_dir/check"
