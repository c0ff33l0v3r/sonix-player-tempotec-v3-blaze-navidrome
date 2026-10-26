#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-navidrome-reportaudio.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    $(pkg-config --cflags alsa freetype2 sdl2) tools/navidrome_reportaudiocheck.c \
    -Wl,--gc-sections -pthread -lm -o "$check_binary"
"$check_binary"
