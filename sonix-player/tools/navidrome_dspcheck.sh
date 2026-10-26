#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-polish-dsp.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -DHOST_BUILD -I. -ffunction-sections -fdata-sections \
    tools/navidrome_dspcheck.c src/system/audio/eq.c src/system/audio/replaygain.c \
    -Wl,--gc-sections -lm -o "$check_binary"
"$check_binary"
