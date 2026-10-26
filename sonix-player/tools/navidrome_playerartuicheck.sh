#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires existing host LVGL objects; no full build is run.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-player-art-ui.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
find build_host/lvgl -name '*.o' -print > "$check_dir/objects"
${CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror \
    -Wno-missing-field-initializers -Wno-unused-function \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_playerartuicheck.c src/gui/nowplaying/cover.c src/gui/shell/icons.c \
    @"$check_dir/objects" -pthread -ldl -lm $(pkg-config --cflags --libs freetype2 sdl2) \
    -Wl,--gc-sections,--wrap=pthread_create -o "$check_dir/check"
"$check_dir/check" 320
"$check_dir/check" 480
