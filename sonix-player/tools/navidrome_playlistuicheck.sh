#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires LVGL objects from an existing host build.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-playlistuicheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_sqlite=build_host/system/db/sqlite3_impl.o
if [ ! -f "$check_sqlite" ]; then
    ${CC:-cc} -std=gnu11 -O1 -g ${NAVIDROME_CHECK_FLAGS:-} -w -I. \
        -c src/system/db/sqlite3_impl.c -o "$check_dir/sqlite.o"
    check_sqlite=$check_dir/sqlite.o
fi
find build_host/lvgl -name '*.o' -print > "$check_dir/lvgl-objects"
python3 - "$check_dir/download-icon.c" <<'PY'
from pathlib import Path
import sys
text = Path('src/gui/shell/icons.c').read_text()
parts = ['#include "src/gui/shell/icons.h"\n']
for name in ('download', 'reorder', 'grip', 'shuffle', 'play_small'):
    start = text.index(f'static const uint8_t icon_{name}_data[] = {{')
    end = text.index('\n};', text.index(f'const lv_image_dsc_t icon_{name} = {{', start)) + 3
    parts.append(text[start:end] + '\n')
Path(sys.argv[1]).write_text(''.join(parts))
parts = []
for name in ('sort_az', 'sort_za', 'sort_year_asc', 'sort_year_desc'):
    start = text.index(f'static const uint8_t icon_{name}_data[] = {{')
    end = text.index('\n};', text.index(f'const lv_image_dsc_t icon_{name} = {{', start)) + 3
    parts.append(text[start:end] + '\n')
Path(sys.argv[1]).with_name('navidrome_sorticons.inc').write_text(''.join(parts))
PY
baseline_flags=
if [ -n "${SONIX_EXPECT_EDGE_FLASH:-}" ]; then baseline_flags=-DNAVIDROME_EDGE_BASELINE; fi
${CC:-cc} $baseline_flags -DNAVIDROME_BROWSE_SOURCE=\"${SONIX_BROWSE_SOURCE:-src/gui/streaming/navidromebrowse.c}\" -std=gnu11 -Wall -Wextra -Werror -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DNAVIDROME_NATIVE_SORT_ICONS -I"$check_dir" -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -Ilvgl \
    tools/navidrome_playlistuicheck.c tools/navidrome_test_support.c "$check_dir/download-icon.c" lvgl/src/font/lv_font_unscii_8.c \
    src/system/streaming/navidromedebug.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c src/system/streaming/navidromebatch.c \
    src/system/streaming/navidromeplaycatalog.c src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/streaming/navidromedownloadstore.c \
    src/system/streaming/navidromestars.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/net/http.c src/system/net/tls.c src/system/core/json.c src/system/core/md5.c "$check_sqlite" \
    @"$check_dir/lvgl-objects" -pthread -ldl -lm $(pkg-config --libs freetype2 sdl2) \
    -Wl,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink,--wrap=sqlite3_step -o "$check_dir/check"
"$check_dir/check" 320
"$check_dir/check" 480
