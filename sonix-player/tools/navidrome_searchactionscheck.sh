#!/bin/sh
# Focused Search-action and shared-consumer regressions; never builds the player.
set -eu
cd "$(dirname "$0")/../.."
check_dir=$(mktemp -d /tmp/sonix-search-actions-regressions.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
run_check() {
    check_label=$1
    shift
    if "$@" > "$check_dir/output" 2>&1; then
        echo "PASS $check_label"
    else
        cat "$check_dir/output" >&2
        echo "FAIL $check_label" >&2
        exit 1
    fi
}
if [ "${1:-}" = --sanitize ]; then
    export ASAN_OPTIONS=detect_leaks=0
    export NAVIDROME_CHECK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    run_check sanitized-capture sh sonix-player/tools/navidrome_actioncheck.sh
    run_check sanitized-queue sh sonix-player/tools/navidrome_playlistcoordinatorcheck.sh
    run_check sanitized-search-ui sh sonix-player/tools/navidrome_searchuicheck.sh
    run_check sanitized-downloads env NAVIDROME_DOWNLOAD_CASES=search,search_remote,search_offline,search_album_offline python3 sonix-player/tools/navidrome_downloadcheck.py
    exit 0
fi
for check_name in action search searchui playliststore playlist playlistui playlistmenu playlistcoordinator favorites star_ui downloadui downloadbrowse browse album offlineui; do
    run_check "$check_name" sh "sonix-player/tools/navidrome_${check_name}check.sh"
done
run_check search-playback env NAVIDROME_SEARCH_PLAYBACK_CHECK=1 sh sonix-player/tools/navidrome_coordinatorcheck.sh
run_check downloads python3 sonix-player/tools/navidrome_downloadcheck.py
