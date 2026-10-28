#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires LVGL objects from an existing host build.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-browsecheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=src/system/db/sqlite3_impl.c
if [ -f build_host/system/db/sqlite3_impl.o ]; then
    check_sqlite=build_host/system/db/sqlite3_impl.o
fi
find build_host/lvgl -name '*.o' -print > "$check_dir/lvgl-objects"
python3 - "$check_dir/navidrome_sorticons.inc" <<'PYICON'
from pathlib import Path
import sys
s = Path('src/gui/shell/icons.c').read_text()
parts = []
for name in ['sort_az', 'sort_za', 'sort_year_asc', 'sort_year_desc']:
    a = s.index(f'static const uint8_t icon_{name}_data[] = {{')
    b = s.index('\n};', s.index(f'const lv_image_dsc_t icon_{name} = {{', a)) + 3
    parts.append(s[a:b])
Path(sys.argv[1]).write_text('\n'.join(parts))
PYICON
${CC:-cc} -DNAVIDROME_NATIVE_SORT_ICONS -I"$check_dir" -std=gnu11 -Wall -Wextra -Werror -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -Ilvgl \
    tools/navidrome_browsecheck.c tools/navidrome_test_support.c src/system/streaming/navidromemembership.c lvgl/src/font/lv_font_unscii_8.c \
    src/system/streaming/navidromedebug.c src/system/device/workactivity.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c \
    src/system/streaming/navidromeplaycatalog.c src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/streaming/navidromestars.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/net/http.c src/system/net/tls.c src/system/core/json.c src/system/core/md5.c "$check_sqlite" \
    @"$check_dir/lvgl-objects" -pthread -ldl -lm $(pkg-config --libs freetype2 sdl2) \
    -Wl,--wrap=http_finite_request,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink -o "$check_dir/check"
SONIX_CHECK_WIDTH=320 "$check_dir/check"
SONIX_CHECK_WIDTH=480 "$check_dir/check"
