#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_objects=${SONIX_LVGL_OBJECTS:-build_host/lvgl}
check_source=${SONIX_LVGL_SOURCE:-lvgl}
if [ ! -f "$check_objects/src/lv_init.o" ]; then
    echo 'Requires existing host LVGL objects; no full build is run.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-player-art-ui.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
find "$check_objects" -name '*.o' -print > "$check_dir/objects"
${CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror \
    -Wno-missing-field-initializers -Wno-unused-function \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -I"$check_source/.." -I"$check_source" \
    tools/navidrome_playerartuicheck.c "$check_source/src/font/lv_font_unscii_8.c" src/gui/nowplaying/cover.c src/gui/shell/icons.c \
    src/gui/streaming/navidromealbum.c src/gui/streaming/navidromethumb.c \
    src/system/image/stb_image_impl.c src/system/image/jpeg_scaled.c \
    src/system/image/png_scaled.c src/system/image/art_shrink.c src/gb/miniz/miniz.c \
    @"$check_dir/objects" -pthread -ldl -lm $(pkg-config --cflags --libs freetype2 sdl2) \
    -Wl,--gc-sections,--wrap=pthread_create -o "$check_dir/check"
PYTHONDONTWRITEBYTECODE=1 python3 - "$check_dir/portrait.png" <<'PYIMAGE'
import sys
from pathlib import Path
sys.path.insert(0, 'tools')
from navidrome_artworkcheck import artwork
Path(sys.argv[1]).write_bytes(artwork(512, 768))
PYIMAGE
"$check_dir/check" 320 480 "$check_dir/portrait.png"
"$check_dir/check" 480 720 "$check_dir/portrait.png"
"$check_dir/check" 480 800 "$check_dir/portrait.png"
