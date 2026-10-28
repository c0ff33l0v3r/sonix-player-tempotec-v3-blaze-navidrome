#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build_host/lvgl/src/lv_init.o ]; then
    echo 'Requires LVGL objects from an existing host build; no check ran.' >&2
    exit 1
fi
check_dir=$(mktemp -d /tmp/sonix-navidrome-sessioncheck.XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
find build_host/lvgl -name '*.o' -print > "$check_dir/objects"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -ffunction-sections -fdata-sections ${NAVIDROME_CHECK_FLAGS:-} \
    -DHOST_BUILD -DNAVIDROME_TEST -DLV_CONF_INCLUDE_SIMPLE -DLV_FONT_UNSCII_8=1 -I. -Ilvgl \
    tools/navidrome_sessioncheck.c tools/navidrome_test_support.c lvgl/src/font/lv_font_unscii_8.c \
    src/system/streaming/navidromeplay.c src/system/streaming/navidromelyrics.c src/system/playback/sessionstore.c src/system/playback/session.c src/system/playback/sessionmusic.c src/system/playback/playlist.c src/system/decode/growfile.c \
    src/system/streaming/streamturn.c src/system/streaming/navidromedebug.c \
    src/system/streaming/navidromeplaycatalog.c src/system/device/workactivity.c src/system/streaming/navidrome.c src/system/streaming/navidromeapi.c \
    src/system/streaming/navidromeplaylists.c src/system/streaming/navidromeplayliststore.c src/system/core/sha256.c \
    src/system/streaming/navidromestars.c src/system/streaming/navidromestore.c src/system/streaming/navidromelibrary.c \
    src/system/streaming/navidromecrypto.c src/system/streaming/navidromevault.c \
    src/system/core/json.c src/system/core/md5.c build_host/system/db/sqlite3_impl.o \
    src/system/streaming/navidromebatch.c src/system/net/http.c src/system/net/tls.c \
    src/system/net/gzip.c src/system/image/png_scaled.c src/gb/miniz/miniz.c \
    @"$check_dir/objects" -pthread -ldl -lm -lresolv $(pkg-config --libs freetype2 sdl2) \
    -Wl,--wrap=http_finite_request,--wrap=http_request,--wrap=http_last_error,--wrap=http_clear_request_metadata,--wrap=http_url_encode,--wrap=http_file_open \
    -Wl,--gc-sections,--wrap=rename,--wrap=fsync,--wrap=unlink,--wrap=malloc,--wrap=calloc -o "$check_dir/check"
session_root="$check_dir/card"
mkdir "$session_root"
mkdir "$check_dir/legacy-card"
"$check_dir/check" legacy "$check_dir/legacy-card"
"$check_dir/check" music "$check_dir/legacy-card"
"$check_dir/check" save "$session_root"
"$check_dir/check" resume "$session_root"
"$check_dir/check" permanent "$session_root"
"$check_dir/check" canonical "$session_root"
"$check_dir/check" boundary "$session_root"
cp -a "$session_root" "$check_dir/clear-card"
"$check_dir/check" clear "$check_dir/clear-card"
"$check_dir/check" empty "$check_dir/clear-card"
cp -a "$session_root" "$check_dir/reenable-card"
"$check_dir/check" reenable "$check_dir/reenable-card"
"$check_dir/check" resume "$check_dir/reenable-card"
"$check_dir/check" failed "$session_root"
"$check_dir/check" resumefail "$session_root"
"$check_dir/check" missingcard "$session_root"
cp -a "$session_root" "$check_dir/reset-card"
"$check_dir/check" reset "$check_dir/reset-card"
cp -a "$session_root" "$check_dir/profile-card"
"$check_dir/check" profile "$check_dir/profile-card"
"$check_dir/check" postfail "$session_root"
cp -a "$session_root" "$check_dir/uncertain-card"
"$check_dir/check" uncertain "$check_dir/uncertain-card"
"$check_dir/check" resume "$check_dir/uncertain-card"
cp -a "$session_root" "$check_dir/position-card"
"$check_dir/check" positionfail "$check_dir/position-card"
cp -a "$session_root" "$check_dir/retire-card"
"$check_dir/check" retirefail "$check_dir/retire-card"
"$check_dir/check" empty "$check_dir/retire-card"
cp -a "$session_root" "$check_dir/allocation-card"
"$check_dir/check" allocfail "$check_dir/allocation-card"
"$check_dir/check" pending "$check_dir/allocation-card"
cp -a "$session_root" "$check_dir/pending-card"
"$check_dir/check" pending "$check_dir/pending-card"
cp -a "$session_root" "$check_dir/corrupt-card"
"$check_dir/check" corrupt "$check_dir/corrupt-card"
cp -a "$session_root" "$check_dir/other-card"
"$check_dir/check" other "$check_dir/other-card"
"$check_dir/check" empty "$check_dir/other-card"
"$check_dir/check" wrongcard "$session_root"
cp -a "$session_root" "$check_dir/disabled-card"
"$check_dir/check" disable "$check_dir/disabled-card"
"$check_dir/check" empty "$check_dir/disabled-card"
cp -a "$session_root" "$check_dir/locked-card"
"$check_dir/check" encrypt "$check_dir/locked-card"
"$check_dir/check" locked "$check_dir/locked-card"
cp -a "$check_dir/locked-card" "$check_dir/unlock-card"
"$check_dir/check" unlock "$check_dir/unlock-card"
cp -a "$check_dir/locked-card" "$check_dir/lateunlock-card"
"$check_dir/check" lateunlock "$check_dir/lateunlock-card"
"$check_dir/check" locked-music "$check_dir/locked-card"
"$check_dir/check" music "$check_dir/locked-card"
cp -a "$session_root" "$check_dir/forgotten-card"
"$check_dir/check" forget "$check_dir/forgotten-card"
"$check_dir/check" empty "$check_dir/forgotten-card"
"$check_dir/check" switch "$session_root"
"$check_dir/check" music "$session_root"
"$check_dir/check" save "$session_root"
"$check_dir/check" resume "$session_root"
