#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires LVGL objects from an existing host build.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-discoveryui.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    check_sqlite="$check_dir/sqlite.o"
    ${CC:-cc} -std=gnu11 -O1 -DHOST_BUILD -I. -c src/system/db/sqlite3_impl.c -o "$check_sqlite"
fi
find build_host/lvgl -name '*.o' -print > "$check_dir/lvgl-objects"
python3 - "$check_dir/refresh-icon.c" <<'PY'
from pathlib import Path
import sys
text = Path('src/gui/shell/icons.c').read_text()
assets = []
for name in ('icon_refresh', 'icon_play_small', 'icon_ellipsis_vertical'):
    start = text.index(f'static const uint8_t {name}_data[] = {{')
    end = text.index('\n};', text.index(f'const lv_image_dsc_t {name} = {{', start)) + 3
    assets.append(text[start:end])
Path(sys.argv[1]).write_text('#include "src/gui/shell/icons.h"\n' + '\n'.join(assets) + '\n')
PY
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -Ilvgl \
    tools/navidrome_discoveryuicheck.c "$check_dir/refresh-icon.c" src/system/streaming/navidromediscovery.c src/gui/shell/gridpage.c tools/navidrome_test_support.c lvgl/src/font/lv_font_unscii_8.c \
    src/system/streaming/navidromesearch.c src/system/streaming/navidromedebug.c src/system/streaming/navidromestars.c src/system/streaming/navidromeplaycatalog.c src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c src/system/streaming/navidromestore.c \
    src/system/streaming/navidromelibrary.c src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/core/json.c src/system/core/md5.c src/system/net/http.c src/system/net/tls.c "$check_sqlite" @"$check_dir/lvgl-objects" \
    -Wl,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink,--wrap=malloc -pthread -ldl -lm $(pkg-config --libs freetype2 sdl2) -o "$check_dir/check"
python3 tools/navidrome_discoverycheck.py "$check_dir/check" 320 480
python3 tools/navidrome_discoverycheck.py "$check_dir/check" 480 800
python3 tools/navidrome_discoverycheck.py "$check_dir/check" 480 720
