#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-navidrome-selftest.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_selftest.c src/system/streaming/navidrome.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/core/json.c src/system/core/md5.c -pthread -ldl \
    -Wl,--wrap=rename,--wrap=fsync,--wrap=unlink -o "$check_binary"
"$check_binary"
