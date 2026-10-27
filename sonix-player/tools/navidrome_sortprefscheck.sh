#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-navidrome-sortprefs.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -I. \
    tools/navidrome_sortprefscheck.c src/system/core/config.c -o "$check_dir/check"
SONIX_CONFIG="$check_dir/settings.ini" "$check_dir/check" write
SONIX_CONFIG="$check_dir/settings.ini" "$check_dir/check" read
