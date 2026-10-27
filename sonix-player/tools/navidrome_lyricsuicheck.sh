#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_objects=${SONIX_LVGL_OBJECTS:-build_host/lvgl}
if [ ! -f "$check_objects/src/lv_init.o" ]; then
    echo 'Requires existing host LVGL objects; no full build is run.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-lyrics-ui.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
find "$check_objects" -name '*.o' -print > "$check_dir/objects"
for check_platform in host device; do
check_platform_flags=
if [ "$check_platform" = host ]; then check_platform_flags=-DHOST_BUILD; fi
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} -DLV_FONT_UNSCII_8=1 $check_platform_flags -DLV_CONF_INCLUDE_SIMPLE -I. -I"${SONIX_LVGL_SOURCE:-lvgl}/.." -I"${SONIX_LVGL_SOURCE:-lvgl}" \
    tools/navidrome_lyricsuicheck.c "${SONIX_LVGL_SOURCE:-lvgl}/src/font/lv_font_unscii_8.c" src/system/library/lyrics.c src/system/streaming/navidromelyricparse.c \
    src/gui/shell/toast.c src/gui/shell/confirm.c src/gui/shell/spinner.c src/gui/shell/icons.c \
    @"$check_dir/objects" -pthread -ldl -lm $(pkg-config --cflags --libs freetype2 sdl2) \
    -Wl,--gc-sections,--wrap=pthread_create -o "$check_dir/check"
if [ -n "${SONIX_CHECK_BINARY:-}" ]; then cp "$check_dir/check" "$SONIX_CHECK_BINARY"; fi
"$check_dir/check" 320 480
"$check_dir/check" 480 800
"$check_dir/check" 480 720
done
