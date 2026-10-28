#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_objects=${SONIX_LVGL_OBJECTS:-build_host/lvgl}
check_source=${SONIX_LVGL_SOURCE:-lvgl}
if [ ! -f "$check_objects/src/lv_init.o" ]; then
    echo 'Requires LVGL objects from an existing host build; no check ran.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-coordinatorcheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
python3 - "$check_dir/veil-icons.c" <<'PYICONS'
from pathlib import Path
import sys
s=Path('src/gui/shell/icons.c').read_text()
out=['#include "src/gui/shell/icons.h"']
for name in ['icon_circle_check','icon_circle_alert','icon_loader_big']:
 start=s.index('static const uint8_t '+name+'_data[] = {')
 end=s.index('\n};',s.index('const lv_image_dsc_t '+name+' = {',start))+3
 out.append(s[start:end])
Path(sys.argv[1]).write_text('\n'.join(out)+'\n')
PYICONS
polish_sources="$check_dir/veil-icons.c"
polish_flags=
if [ "${NAVIDROME_POLISH_QUEUE_UI:-0}" = 1 ]; then
    polish_sources='tools/navidrome_queue_uifixture.c tools/navidrome_cover_test_support.c src/gui/nowplaying/coverloader.c src/gui/streaming/navidromeart.c src/system/image/stb_image_impl.c'
    polish_flags='-DNAVIDROME_POLISH_QUEUE_UI -Wno-missing-field-initializers'
fi
find "$check_objects" -name '*.o' -print > "$check_dir/objects"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -ffunction-sections -fdata-sections $polish_flags ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -I"$check_source/.." -I"$check_source" -Isrc/system/streaming \
    $polish_sources tools/navidrome_preparationuicheck.c tools/navidrome_test_support.c "$check_source/src/font/lv_font_unscii_8.c" \
    ${SONIX_PREPARATION_SOURCE:-src/system/streaming/navidromeplay.c} src/system/playback/sessionstore.c src/system/playback/playlist.c src/system/decode/growfile.c \
    src/system/streaming/navidromecache.c src/system/streaming/streamturn.c src/system/streaming/navidromedebug.c \
    src/system/streaming/navidromeplaycatalog.c src/system/device/workactivity.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c \
    src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/streaming/navidromestars.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/core/json.c src/system/core/md5.c "${SONIX_SQLITE_OBJECT:-build_host/system/db/sqlite3_impl.o}" \
    src/system/streaming/navidromebatch.c src/system/net/http.c src/system/net/tls.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c \
    @"$check_dir/objects" -pthread -ldl -lm -lresolv $(pkg-config --libs freetype2 sdl2) \
    -Wl,--wrap=http_finite_request,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open \
    -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink,--wrap=malloc -o "$check_dir/check"
if [ "${NAVIDROME_SEARCH_PLAYBACK_CHECK:-0}" = 1 ]; then
    python3 tools/navidrome_searchplaycheck.py "$check_dir/check"
else
    "$check_dir/check"
fi
