#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-navidrome-streamcheck.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_streamcheck.c src/system/streaming/navidromeapi.c \
    src/system/device/workactivity.c src/system/streaming/navidrome.c src/system/streaming/navidromecrypto.c \
    src/system/core/md5.c src/system/image/png_scaled.c src/system/net/gzip.c src/gb/miniz/miniz.c src/system/net/http.c src/system/net/tls.c \
    -Wl,--gc-sections -pthread -ldl -lresolv -o "$check_binary"
python3 tools/navidrome_streamcheck.py "$check_binary"
