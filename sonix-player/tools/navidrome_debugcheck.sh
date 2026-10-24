#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-navidrome-debugcheck.XXXXXX)
check_log=$(mktemp /tmp/sonix-navidrome-debug-general.XXXXXX)
trap 'rm -f "$check_binary" "$check_log"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -DND_DEBUG_FILE_BYTES=4096 -I. \
    tools/navidrome_debugcheck.c src/system/streaming/navidromedebug.c \
    src/system/core/logging.c src/system/core/config.c \
    -pthread -Wl,--wrap=write,--wrap=time -o "$check_binary"
SONIX_LOG="$check_log" "$check_binary"
cat "$check_log"
