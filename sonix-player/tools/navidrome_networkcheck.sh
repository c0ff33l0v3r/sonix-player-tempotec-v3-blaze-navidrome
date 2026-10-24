#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-navidrome-networkcheck.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_networkcheck.c src/system/streaming/navidrome.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/core/json.c src/system/core/md5.c src/system/core/config.c src/system/core/logging.c \
    src/system/net/http.c src/system/net/tls.c -pthread -ldl -lresolv -o "$check_binary"
python3 tools/navidrome_networkcheck.py "$check_binary"
