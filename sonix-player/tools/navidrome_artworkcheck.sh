#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-artworkcheck.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_artworkcheck.c src/gui/nowplaying/cover.c \
    src/system/image/stb_image_impl.c src/system/image/jpeg_scaled.c \
    src/system/image/png_scaled.c src/system/image/art_shrink.c \
    src/gb/miniz/miniz.c src/system/streaming/navidromeapi.c \
    src/system/streaming/navidromecrypto.c src/system/core/md5.c \
    src/system/device/workactivity.c src/system/net/http.c src/system/net/tls.c src/system/net/gzip.c \
    -Wl,--gc-sections,--wrap=cover_load_image_memory,--wrap=malloc,--wrap=write,--wrap=getaddrinfo -lm -pthread -ldl -lresolv -o "$check_binary"
python3 tools/navidrome_artworkcheck.py "$check_binary"
