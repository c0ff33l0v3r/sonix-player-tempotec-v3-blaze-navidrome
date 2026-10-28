#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_root=$(mktemp -d /tmp/sonix-navidrome-debugcheck.XXXXXX)
trap 'rm -rf "$check_root"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -DLOG_ROTATE_BYTES=4096 -DNAVIDROME_DOWNLOAD_PUBLISH_TEST -I. \
    tools/navidrome_debugcheck.c src/system/streaming/navidromedebug.c \
    src/system/core/logging.c src/system/core/config.c src/system/streaming/navidromedownloadfile.c \
    -pthread -Wl,--wrap=write,--wrap=clock_gettime,--wrap=fopen -o "$check_root/check"
mkdir "$check_root/card" "$check_root/explicit"
"$check_root/check" "$check_root/card" card
"$check_root/check" "$check_root/explicit" explicit
