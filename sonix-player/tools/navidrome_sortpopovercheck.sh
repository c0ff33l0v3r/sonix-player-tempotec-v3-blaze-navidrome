#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires existing host LVGL objects; no check ran.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-sort-popover.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
find build_host/lvgl -name '*.o' -print > "$check_dir/objects"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DLV_FONT_UNSCII_8=1 -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_sortpopovercheck.c src/system/core/frameprof.c src/gui/shell/icons.c lvgl/src/font/lv_font_unscii_8.c \
    @"$check_dir/objects" -Wl,--gc-sections -pthread -lm $(pkg-config --libs freetype2 sdl2) -o "$check_dir/check"
"$check_dir/check" 320
"$check_dir/check" 480
