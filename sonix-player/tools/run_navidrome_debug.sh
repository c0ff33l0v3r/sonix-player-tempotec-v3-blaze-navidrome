#!/bin/sh
# Uses the existing timestamped/asynchronous logger. No build is run here.
set -eu
cd "$(dirname "$0")/.."
navidrome_debug_card=${SONIX_SD_ROOT:-"$PWD/sd_card"}
mkdir -p "$navidrome_debug_card"
navidrome_debug_log="$navidrome_debug_card/navidrome.log"
# Keep the previous session: SONIX_LOG starts a new file at launch.
if [ -f "$navidrome_debug_log" ]; then
    mv -f "$navidrome_debug_log" "$navidrome_debug_log.old"
fi
printf 'Writing player/Navidrome diagnostics to %s\n' "$navidrome_debug_log"
export SONIX_SD_ROOT="$navidrome_debug_card"
export SONIX_LOG="$navidrome_debug_log"
exec ./sonix_player_host "$@"
