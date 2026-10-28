#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# Compile production Search independently: fixture includes must not supply a
# missing service declaration before the production source is included.
${CC:-cc} -std=gnu11 -O3 -Wall -Werror=implicit-function-declaration \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -D_FILE_OFFSET_BITS=64 -I. -Ilvgl \
    -fsyntax-only src/gui/streaming/navidromesearch.c
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires LVGL objects from an existing host build.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-searchui.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    check_sqlite="$check_dir/sqlite.o"
    ${CC:-cc} -std=gnu11 -O1 -DHOST_BUILD -I. -c src/system/db/sqlite3_impl.c -o "$check_sqlite"
fi
find build_host/lvgl -name '*.o' -print > "$check_dir/lvgl-objects"
python3 - "$check_dir/navidrome_actionicons.inc" <<'PYICON'
from pathlib import Path
import sys
text = Path('src/gui/shell/icons.c').read_text()
parts = []
for name in ('download', 'album_big', 'shuffle', 'list_queue', 'star_plus', 'close', 'list_plus', 'ellipsis_vertical', 'check'):
    first = text.index(f'static const uint8_t icon_{name}_data[] = {{')
    last = text.index('\n};', text.index(f'const lv_image_dsc_t icon_{name} = {{', first)) + 3
    parts.append(text[first:last] + '\n')
Path(sys.argv[1]).write_text(''.join(parts))
PYICON
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DNAVIDROME_NATIVE_ACTION_ICONS -I"$check_dir" -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -Ilvgl \
    tools/navidrome_searchuicheck.c tools/navidrome_test_support.c src/system/streaming/navidromemembership.c lvgl/src/font/lv_font_unscii_8.c \
    src/system/streaming/navidromesearch.c src/system/streaming/navidromeactions.c src/system/streaming/navidromeplaycatalog.c src/system/streaming/navidromedebug.c src/system/streaming/navidromestars.c src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/device/workactivity.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c src/system/streaming/navidromestore.c \
    src/system/streaming/navidromelibrary.c src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/core/json.c src/system/core/md5.c src/system/net/http.c src/system/net/tls.c "$check_sqlite" @"$check_dir/lvgl-objects" \
    -Wl,--wrap=http_finite_request,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink,--wrap=malloc -pthread -ldl -lm $(pkg-config --libs freetype2 sdl2) -o "$check_dir/check"
"$check_dir/check" 320
"$check_dir/check" 480 720
"$check_dir/check" 480 800
