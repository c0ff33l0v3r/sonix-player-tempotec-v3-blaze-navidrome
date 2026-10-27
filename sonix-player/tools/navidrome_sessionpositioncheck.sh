#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-sessionposition.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Wno-misleading-indentation -O1 -g -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl tools/navidrome_sessionpositioncheck.c -Wl,--gc-sections -lm -pthread -o "$check_dir/check"
"$check_dir/check"
