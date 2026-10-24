"""Check launcher paths with a synthetic executable; never launch the player."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    source = Path(__file__).with_name('run_navidrome_debug.sh')
    subprocess.run(['sh', '-n', str(source)], check=True)
    with tempfile.TemporaryDirectory(prefix='sonix launcher check ') as temp:
        root = Path(temp) / 'player with spaces'
        tools = root / 'tools'
        tools.mkdir(parents=True)
        launcher = tools / source.name
        shutil.copyfile(source, launcher)
        binary = root / 'sonix_player_host'
        binary.write_text('#!/bin/sh\nprintf "%s\\n" "$PWD" "$SONIX_CONFIG" "$SONIX_SD_ROOT" "$SONIX_LOG"\n')
        binary.chmod(0o700)
        env = dict(os.environ)
        for key in ('SONIX_CONFIG', 'SONIX_SD_ROOT', 'SONIX_LOG'):
            env.pop(key, None)
        result = subprocess.run(['sh', str(launcher)], cwd='/', env=env, check=True, capture_output=True, text=True)
        lines = result.stdout.splitlines()[-4:]
        assert lines == [str(root), str(root / 'config.ini'), str(root / 'sd_card'), str(root / 'sd_card/navidrome.log')]
        old = root / 'sd_card/navidrome.log'
        old.write_text('previous synthetic log')
        env['SONIX_CONFIG'] = ''
        subprocess.run(['sh', str(launcher)], cwd='/', env=env, check=True, capture_output=True)
        assert old.with_name('navidrome.log.old').read_text() == 'previous synthetic log'
        config = Path(temp) / 'custom configuration.ini'
        card = Path(temp) / 'custom card'
        env.update(SONIX_CONFIG=str(config), SONIX_SD_ROOT=str(card))
        result = subprocess.run(['sh', str(launcher)], cwd='/', env=env, check=True, capture_output=True, text=True)
        assert result.stdout.splitlines()[-4:] == [str(root), str(config), str(card), str(card / 'navidrome.log')]
        assert not config.exists()  # Only the mock binary ran; no setup or credentials were touched.
    print('Launcher syntax, absolute config defaults, spaces/overrides, working directory and log rotation: passed')


if __name__ == '__main__':
    main()
