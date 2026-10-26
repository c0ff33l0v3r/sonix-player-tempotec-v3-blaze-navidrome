#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires existing host LVGL objects; no full build is run.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-download-ui.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
find build_host/lvgl -name '*.o' -print > "$check_dir/objects"
${CC:-cc} -std=gnu11 -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 \
    -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} -I. -Ilvgl \
    tools/navidrome_downloaduicheck.c src/gui/streaming/navidromedownloadmenu.c \
    src/gui/streaming/navidromethumb.c src/gui/shell/icons.c src/gui/library/folderpicker.c src/gui/shell/settingsrow.c src/gui/shell/keyboard.c src/system/input/kblayout.c src/system/core/config.c \
    src/system/streaming/navidromedownloadstore.c src/system/streaming/navidromeaudio.c \
    lvgl/src/font/lv_font_unscii_8.c @"$check_dir/objects" \
    -pthread -ldl -lm $(pkg-config --libs freetype2 sdl2) \
    -Wl,--gc-sections,--wrap=config_save -o "$check_dir/check"
"$check_dir/check" 320
"$check_dir/check" 480
