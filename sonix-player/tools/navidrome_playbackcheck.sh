#!/bin/sh
set -eu
check_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$check_root"
sh sonix-player/tools/navidrome_queuecheck.sh
sh sonix-player/tools/navidrome_streamcheck.sh
sh sonix-player/tools/navidrome_audiocheck.sh
sh sonix-player/tools/navidrome_coordinatorcheck.sh
sh sonix-player/tools/host_audiodraincheck.sh
