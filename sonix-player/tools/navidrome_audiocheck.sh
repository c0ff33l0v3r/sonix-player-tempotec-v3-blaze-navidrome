#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build_host/system/decode/opusdec.o ]; then
    echo 'Requires ancillary decoder objects from an existing host build; no check ran.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-audiocheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
for check_object in build_host/system/decode/*.o build_host/system/decode/alac/*.o build_host/system/decode/ape/*.o; do
    case "$check_object" in */decode.o|*/growfile.o) continue;; esac
    printf '%s\n' "$check_object" >> "$check_dir/objects"
done
${CC:-cc} -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl $(pkg-config --cflags opusfile wavpack) \
    tools/navidrome_audiocheck.c src/system/streaming/navidromeaudio.c \
    src/system/streaming/navidromeapi.c src/system/streaming/navidromecrypto.c src/system/streaming/streamturn.c \
    src/system/core/md5.c src/system/device/workactivity.c src/system/image/png_scaled.c src/system/net/gzip.c src/gb/miniz/miniz.c src/system/net/http.c src/system/net/tls.c \
    src/system/decode/decode.c src/system/decode/growfile.c \
    build_host/system/library/cue.o build_host/system/core/utils.o @"$check_dir/objects" \
    -Wl,--gc-sections,--wrap=write,--wrap=close,--wrap=statvfs -pthread -ldl -lresolv -lm $(pkg-config --libs opusfile wavpack) -o "$check_dir/check"
python3 tools/navidrome_audiocheck.py "$check_dir/check"
