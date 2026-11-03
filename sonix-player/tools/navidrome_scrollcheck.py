#!/usr/bin/env python3
"""Real LVGL delayed-window regressions; --baseline extracts a19cd44."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='sonix-scroll-') as tmp:
        env = os.environ.copy()
        flags = env.get('NAVIDROME_CHECK_FLAGS', '') + ' -DNAVIDROME_VIEWPORT_PROGRESS_CHECK'
        if args.baseline:
            source = Path(tmp)/'browse.c'
            source.write_bytes(subprocess.check_output(['git', 'show', 'a19cd44:sonix-player/src/gui/streaming/navidromebrowse.c'], cwd=repo))
            flags += ' -Isrc/gui/streaming -DNAVIDROME_VIEWPORT_BASELINE -DNAVIDROME_BROWSE_SOURCE="' + str(source) + '"'
        env['NAVIDROME_CHECK_FLAGS'] = flags
        env['SONIX_SCROLL_CHECK_THREE_SIZES'] = '1'
        result = subprocess.run(['sh', 'sonix-player/tools/navidrome_browsecheck.sh'], cwd=repo, env=env, capture_output=True, text=True, timeout=100)
        if args.baseline:
            assert result.returncode and 'viewport motion and slow-read observation must not supersede' in result.stderr, result.stderr[-4000:]
            print('a19cd44 delayed viewport supersession defect reproduced')
        else:
            assert result.returncode == 0, result.stderr[-4000:] + result.stdout[-4000:]
            assert result.stdout.count('delayed production viewport reads:') == 3
            print(result.stdout)

if __name__ == '__main__':
    main()
