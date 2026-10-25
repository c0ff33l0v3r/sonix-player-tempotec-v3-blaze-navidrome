#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-navidrome-sync.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
if [ -f build_host/system/db/sqlite3_impl.o ]; then
    check_sqlite=build_host/system/db/sqlite3_impl.o
else
    check_sqlite="$check_dir/sqlite.o"
    ${CC:-cc} -std=gnu11 -O1 -g -I. -c src/system/db/sqlite3_impl.c -o "$check_sqlite"
fi
check_prefix=
check_transport="src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c src/system/streaming/navidromebatch.c"
for check_arg in "$@"; do
    if [ "$check_arg" = --baseline ]; then
        python3 tools/navidrome_syncbaseline.py "$check_dir/baseline"
        check_prefix="$check_dir/baseline/"
        check_transport=
    fi
done
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -I"${check_prefix:-.}" -I. -Ilvgl \
    tools/navidrome_synccheck.c tools/navidrome_test_support.c \
    "${check_prefix}src/system/streaming/navidromedebug.c" "${check_prefix}src/system/streaming/navidrome.c" \
    "${check_prefix}src/system/streaming/navidromeapi.c" "${check_prefix}src/system/streaming/navidromestore.c" \
    "${check_prefix}src/system/streaming/navidromelibrary.c" "${check_prefix}src/system/streaming/navidromecrypto.c" \
    "${check_prefix}src/system/streaming/navidromevault.c" "${check_prefix}src/system/core/json.c" src/system/core/md5.c \
    $check_transport "${check_prefix}src/system/net/http.c" "${check_prefix}src/system/net/tls.c" "$check_sqlite" \
    -lm -pthread -ldl -lresolv -o "$check_dir/synccheck"
if [ "${NAVIDROME_STACK_CHECK:-0}" = 1 ]; then
    python3 - "$check_dir" <<'STACK'
from pathlib import Path
import sys
records = []
for path in Path(sys.argv[1]).glob('*.su'):
    for line in path.read_text().splitlines():
        fields = line.split('\t')
        if len(fields) > 2 and fields[1].isdigit():
            records.append((int(fields[1]), fields[0], fields[2]))
selected = sorted(records, reverse=True)[:12]
selected += [r for r in records if any(key in r[1] for key in (':value', ':object', ':string', ':sync_fetch', ':finite_resolver', ':nd_batch_read')) and r not in selected]
for size, name, kind in selected:
    print(f'host_stack_frame_bytes={size} function={name} kind={kind}', file=sys.stderr)
STACK
fi
python3 tools/navidrome_synccheck.py "$check_dir/synccheck" "$@"
