#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-playlist-menucheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Wno-missing-field-initializers -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_playlistmenucheck.c -Wl,--gc-sections -o "$check_dir/check"
"$check_dir/check"
