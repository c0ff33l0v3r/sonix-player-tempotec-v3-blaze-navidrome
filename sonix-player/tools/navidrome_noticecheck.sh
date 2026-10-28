#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_binary=$(mktemp /tmp/sonix-notice-check.XXXXXX)
trap 'rm -f "$check_binary"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -ffunction-sections -fdata-sections \
    ${NAVIDROME_CHECK_FLAGS:-} -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    tools/navidrome_noticecheck.c -Wl,--gc-sections -pthread -o "$check_binary"
"$check_binary"
