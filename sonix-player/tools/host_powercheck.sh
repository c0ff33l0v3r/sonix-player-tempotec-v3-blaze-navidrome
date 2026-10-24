#!/bin/sh
# Inspect compiled host code. Never invoke a power action or boot a simulator.
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-host-powercheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
for source in src/gui/shell/powermenu.c src/system/device/power.c; do
    ${CC:-cc} -std=gnu11 -O0 -ffunction-sections -DHOST_BUILD \
        -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl -c "$source" -o "$check_dir/$(basename "$source").o"
done
${OBJDUMP:-objdump} -r -j .text.do_reboot "$check_dir/powermenu.c.o" > "$check_dir/reboot-relocations"
${OBJDUMP:-objdump} -r -j .text.power_shutdown "$check_dir/power.c.o" > "$check_dir/shutdown-relocations"
python3 - "$check_dir" <<'PY'
from pathlib import Path
import re
import sys
for path in Path(sys.argv[1]).glob('*-relocations'):
    text = path.read_text()
    # These entry points must not call the OS, release storage, blank the
    # display or perform any device shutdown cleanup in a host build.
    symbols = re.findall(r'R_\w+\s+(\S+)', text)
    assert symbols and all(symbol.startswith(('.rodata', 'puts', 'printf')) for symbol in symbols), (path.name, symbols)
print('Host reboot/shutdown contain diagnostic output only; no power action executed.')
PY
