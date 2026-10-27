#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/sonix-sessionstorecheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -O1 -g ${NAVIDROME_CHECK_FLAGS:-} -w -I. -c src/system/db/sqlite3_impl.c -o "$check_dir/sqlite.o"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O1 -g ${NAVIDROME_CHECK_FLAGS:-} -I. tools/navidrome_sessionstorecheck.c src/system/playback/sessionstore.c "$check_dir/sqlite.o" -pthread -ldl -lm -Wl,--wrap=sqlite3_open_v2,--wrap=sqlite3_close,--wrap=sqlite3_step,--wrap=malloc,--wrap=fsync,--wrap=rename -o "$check_dir/check"
"$check_dir/check"
