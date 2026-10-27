#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-host-gaplesscheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
check_lvgl=${SONIX_LVGL_ROOT:-"$PWD/lvgl"}
if [ ! -f "$check_lvgl/lvgl.h" ]; then
    echo 'Requires LVGL headers; set SONIX_LVGL_ROOT to an existing dependency checkout. No check ran.' >&2
    exit 1
fi
check_audio=${HOST_GAPLESS_AUDIO_SOURCE:-src/system/audio/audio.c}
ulimit -c 0
for check_mode in host non-host; do
    if [ "$check_mode" = host ]; then
        check_flags=-DHOST_BUILD
    else
        check_flags=-DHOST_GAPLESS_TARGET
    fi
    printf 'Checking %s production audio branch\n' "$check_mode"
    ${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections ${HOST_GAPLESS_CHECK_FLAGS:-} \
        $check_flags -DLV_CONF_INCLUDE_SIMPLE -I. -I"$check_lvgl/.." -I"$check_lvgl" \
        "-DHOST_GAPLESS_AUDIO_SOURCE=\"$check_audio\"" \
        $(pkg-config --cflags alsa freetype2 sdl2) tools/host_gaplesscheck.c \
        -Wl,--gc-sections -pthread -lm -lasound -o "$check_dir/check"
    if ! "$check_dir/check" 2> "$check_dir/audio.log"; then
        cat "$check_dir/audio.log" >&2
        exit 1
    fi
    python3 - "$check_dir/audio.log" <<'PY'
from pathlib import Path
import sys
log = Path(sys.argv[1]).read_text()
for flag in ['format=1 device=0 route=0', 'format=0 device=1 route=0', 'format=0 device=0 route=1', 'output_state=1']:
    assert flag in log, flag
for line in log.splitlines():
    if 'gapless: reuse rejected' in line:
        assert 'fixture' not in line and '/' not in line.split('(')[0], line
print('Safe handoff rejection diagnostics distinguish format/device/route/output-state failures.')
PY
done
