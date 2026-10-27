#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_objects=${SONIX_LVGL_OBJECTS:-build_host/lvgl}
check_source=${SONIX_LVGL_SOURCE:-lvgl}
if [ ! -f "$check_objects/src/lv_init.o" ]; then
    echo 'Requires existing host LVGL objects; no full build is run.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-lyrics-routing.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
python3 - "$check_dir/back-census.h" <<'PYCENSUS'
from pathlib import Path
import re, sys
counts = {}
for p in Path('src/gui').rglob('*.c'):
    if p.name == 'switcher.c': continue
    n = len(re.findall(r'\bswitcher_set_back_guard\s*\(', p.read_text()))
    if n: counts[str(p)] = n
for name, var, expected in [('streaming/navidromebrowse.c','level',3), ('streaming/navidromeplaylists.c','page',4)]:
    p = 'src/gui/' + name
    assert counts[p] == 1
    assert re.search(r'for \(int '+var+r' = 0; '+var+r' < '+str(expected)+r';', Path(p).read_text())
    counts[p] = expected
p = 'src/gui/library/medialist.c'
assert counts[p] == 1
panels = re.search(r'panel_t \*const panels\[\] = \{([^}]+)\}', Path(p).read_text())
assert panels
counts[p] = len(panels.group(1).split(','))
total = sum(counts.values())
Path(sys.argv[1]).write_text('#define CURRENT_BACK_GUARDS '+str(total)+'\n')
print('Current guard census:', total, 'screens across', len(counts), 'source files')
PYCENSUS
find "$check_objects" -name '*.o' -print > "$check_dir/objects"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 \
    -I. -I"$check_source/.." -I"$check_source" -I"$check_dir" \
    tools/navidrome_lyricsroutingcheck.c "$check_source/src/font/lv_font_unscii_8.c" \
    @"$check_dir/objects" -pthread -ldl -lm $(pkg-config --cflags --libs freetype2 sdl2) -Wl,--gc-sections -o "$check_dir/check"
"$check_dir/check" 320 480 ${SONIX_BACK_BASELINE:+baseline}
if [ -z "${SONIX_BACK_BASELINE:-}" ]; then
    "$check_dir/check" 480 800
    "$check_dir/check" 480 720
fi
