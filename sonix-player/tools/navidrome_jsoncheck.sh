#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-navidrome-jsoncheck.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -I. tools/navidrome_jsoncheck.c \
    src/system/core/json.c -Wl,--wrap=realloc -o "$check_binary"
"$check_binary"
