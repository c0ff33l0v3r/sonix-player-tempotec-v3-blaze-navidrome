#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# Reuse LVGL objects from the user's host build; never run a full player build.
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires LVGL objects from an existing host build.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-uicheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
find build_host/lvgl -name '*.o' -print > "$check_dir/lvgl-objects"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    -DLV_FONT_UNSCII_8=1 tools/navidrome_uicheck.c lvgl/src/font/lv_font_unscii_8.c \
    src/system/streaming/navidrome.c src/system/streaming/navidromecrypto.c \
    src/system/streaming/navidromevault.c src/system/core/json.c src/system/core/md5.c \
    @"$check_dir/lvgl-objects" -pthread -ldl -lm $(pkg-config --libs freetype2 sdl2) \
    -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink -o "$check_dir/check"
"$check_dir/check"
