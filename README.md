# Sonix Player for the TempoTec V3 Blaze

A port of [Sonix Player](https://github.com/Jepl4r/sonix-player) by
[@Jepl4r](https://github.com/Jepl4r) to the **TempoTec V3 Blaze**
(`V3_ANALOG_2025`). It replaces the stock music player and keeps TempoTec's
kernel, drivers and recovery. Everything Sonix does on the HiBy R3 Pro II and
R1 is here, laid out for the Blaze's 320x480 panel and its AK4493 DAC, plus the
fixes and speed-ups listed below.

Only for the **V3 Blaze**, not the older V3 Analog. Flashing always carries
some risk: read [Going back to stock and recovery](#going-back-to-stock-and-recovery)
first.

The upstream README, with the full build documentation and the source tree, is
in [UPSTREAM-README.md](UPSTREAM-README.md). Features are in
[FEATURES.md](FEATURES.md).

The released images carry Last.fm keys only, so Tidal, Qobuz and podcasts stay
off in them; see [Building](#building) to make an image with keys of your own.

## Install

1. Charge the player above 30 %.
2. **Keep a copy of the official firmware.** If the root of your microSD
   already has a `v3_analog_2025.upt` (the stock OTA leaves one there), rename
   it to `v3_analog_2025.upt.stock` and copy it to your computer as well.
   Remove any `update.upt`: the updater prefers it over every other name.
3. Download `v3_analog_2025.upt` from the
   [latest release](https://github.com/carmine-bin/sonix-player-tempotec-v3-blaze/releases/latest)
   and check its SHA-256 against the release notes.
4. Copy it to the **root of the microSD card**, keeping the name
   `v3_analog_2025.upt`.
5. Start the update:
   - **from the stock firmware**: Settings → Firmware update → Update via
     micro SD card;
   - **from Sonix**: Settings → System → Update firmware → From SD card.
6. Let it flash and reboot by itself.

## Updates over the internet

Once Sonix is installed, new versions arrive without visiting this page:
Settings → System → Update firmware → **Via internet** (Wi-Fi on). The player
looks at the releases of this repository, offers the newest one above the
version it runs, shows its notes, downloads `v3_analog_2025.upt` to the card,
checks its SHA-256 and installs it.

The download is written to the card root as `v3_analog_2025.upt`, so keep your
copy of the official firmware under another name (`.upt.stock`).

Versions read `<upstream version>.<Blaze revision>-blaze`: `1.2.0.1-blaze` is
upstream Sonix 1.2.0 with the first set of Blaze changes.

## What is different on the Blaze

**Hardware**

- The interface uses the native 320x480 layout, with shared size/font helpers
  and icons generated for the 320 px wide panel; touch coordinates match it.
- AK4493SEQ audio board: three gain levels like the stock player (LDB, MDB,
  HDB), TempoTec's default DAC filter, the analogue output muted before
  power-off and reboot, the amplifier left off until the first track.
- The hardware buttons the right way round, TempoTec's boot logo, the button
  list page.

**Bluetooth**

- `main.conf` with `ControllerMode=bredr` and `FastConnectable=false`: the
  receiver range fix from the
  [V3 Blaze mod](https://github.com/carmine-bin/tempotec-v3-blaze-mod)
  (LDAC 990 kbps holds).
- Receiver: aptX HD decoded correctly (it used to be noise).
- Receiver: a paired phone comes back without pairing again. Opening the
  receiver invites every paired phone or computer, starts on the best codec
  the phone offers (LDAC first), and lets go of it when the receiver closes.
  Tapping a phone in the list links it without audio.

**Speed and battery**

- Covers: a persistent RGB565 cache, decoded off the interface thread; the old
  cover stays up until the next is ready, and follows a skip at once.
- Skips: the audio output stays open, and the next tracks' tags, favourite
  star and glyphs are ready before the press (skip-to-sound about 46 ms).
- Lists: fewer database reads while scrolling, the first rows bound first,
  the letters of the library's names warmed in the background.
- Start-up: OpenSSL and the code pinning off the path to the first frame
  (first frame 4.4 s → 3.2 s).
- Waking the screen is instant inside a minute after it went off (about
  17 ms), with no black frame.
- Hidden spinners, marquees and carets stop; with the screen off and music
  playing the interface wakes about 7 times a second instead of 44.

**Interface**

- AMOLED theme beside Dark and Light.
- Now playing: Show artist, beside Show album.
- About names the DAC as the AK4493SEQ.

The full list is the commit history of the
[`blaze-port`](https://github.com/carmine-bin/sonix-player-tempotec-v3-blaze/commits/blaze-port)
branch.

## Navidrome browsing and playback

Open Streaming → Navidrome → Settings → **Navidrome server** to edit Host,
Username, and Password, save/connect, or Forget Server. Host is the complete
API base URL; include a path such as `/navidrome` only if the server actually
serves its API there. Editing Host or Username preserves your password draft;
Back discards unsubmitted edits.

**Encrypt Credentials** is optional and off for new profiles: the server password
is then saved unencrypted in the restricted credential file on internal storage.
Existing encrypted profiles stay encrypted. Enabling encryption uses a separate
Unlock Password (at least six characters); encrypted profiles need unlocking after
restart. Disabling encryption requires available credentials or the correct Unlock
Password. Changing protection keeps the same library and does not contact the server.

From the main Navidrome settings page, use
**Sync Library** to copy track, album, artist, and genre metadata to the selected
SD card. Successful Save & Connect and Reconnect operations also offer
**Sync Now** or **Later** when main settings is next visible. A new sync needs writable card storage; cancelling or
failing a refresh keeps the last successful library.

After syncing, those four categories browse the local index, including offline
once credentials are available (after unlock in encrypted mode). Settings shows
connection status separately from the last sync time and library counts.
Thumbnails download as they become visible and cached thumbnails remain
available offline. The metadata and artwork cache lives under
`.local/navidrome/.cache/` on the card; credentials remain in the separate internal
credential record in the selected protection mode. Name-sorted lists of at least
30 entries show a letter strip while scrolling: tap or slide to jump, then it
hides after inactivity. Album track lists retain disc/track order.

**Debug mode** records redacted Navidrome diagnostics to
`.local/navidrome-debug-YYYYMMDD-HHMMSS-NNN.log` on the selected card. Settings
shows the path or unavailable-storage reason. Files rotate at 2 MiB with at most
five retained; disabling stops recording and keeps the files. Passwords, usernames,
tokens, full request URLs, and raw responses are excluded. It operates independently
of the debug launcher's full-player `navidrome.log`.

Tap a track to play it in the existing Now Playing screen. Album selections
queue the complete album in disc/track order; Tracks selections queue the whole
Tracks list, starting at the selected entry. Next/Previous, repeat, shuffle, and
queue edits use the existing Music behavior. Browsing away or refreshing the
library preserves the captured queue. Queues containing Navidrome entries are
session-only and are not restored after restart.

Playback requests the original audio without transcoding. MP3 and FLAC can start
during download; other supported local formats finish downloading first. Seeking
while downloading stays within confirmed available audio; unknown final lengths
use conservative seek limits. Startup time and buffering depend on the format,
tags, server, and network. No range-based remote seeking is provided.

Original audio is retained under `.local/navidrome/.cache/audio/` on the selected card,
with a 1 GiB budget and a 16 MiB free-space reserve. Only the current track and up
to two upcoming tracks are prepared ahead. Eligible old entries and abandoned
partials are reclaimed; these are disposable cache files, not pinned downloads.
Complete matching files can play offline, including from a read-only card, once
credentials and the matching library are available. Cache misses require an
explicit existing connection and writable card; selecting a track does not
reconnect automatically. Lock/Forget, credential replacement, and card changes
cancel inappropriate playback or preparation. Audio on the card is not encrypted
by Encrypt Credentials. Existing metadata, artwork, and audio caches move
into `.local/navidrome/.cache/` when the card is writable. Read-only legacy cards
remain usable until they can migrate. Credentials and diagnostic logs retain
their existing locations.

Show album opens the exact Navidrome album. Show artist is unavailable when the
snapshot has no authoritative primary artist ID. Server favourites, playlists,
search results, lyrics integration, and listening reports remain deferred.

### Blaze simulator

Build from `sonix-player/` with matching icon dimensions:

```sh
make host -j12 PANEL_WIDTH=320
```

From the repository root, launch the existing binary with:

```sh
SONIX_PANEL=320x480 sh sonix-player/tools/run_navidrome_debug.sh
```

The launcher defaults to `sonix-player/config.ini` for persistent setup/settings,
`sonix-player/sd_card/` for the simulated card, and `navidrome.log` there (the
previous session becomes `.old`). It does not build. Nonempty `SONIX_CONFIG` and
`SONIX_SD_ROOT` override those paths; absolute paths avoid working-directory ambiguity.
Keep the same configuration directory to keep using its adjacent credential record.
Direct binary launches without `SONIX_CONFIG` retain ordinary settings only for
that session. `SONIX_PANEL` controls the runtime layout; it cannot resize icons
baked into a binary built with a different `PANEL_WIDTH`.

New credential writes use version 2, while existing version-1 encrypted records
remain readable. Older player binaries cannot read version-2 credentials; a
rollback requires a user-owned compatible backup or re-entering the server
credentials. No plaintext backup is created automatically.

## Going back to stock and recovery

**Back to stock:** put the official firmware in the card root as
`v3_analog_2025.upt` and install it from Settings → System → Update firmware →
From SD card. The official v1.2 is on
[TempoTec's firmware page](https://www.tempotec.net/pages/firmware-download)
("TempoTec V3 Blaze Firmware V1.2", not the V3 V1.0); v1.3 comes over the air
from there.

**If the player does not boot:** with the player off, hold **Power + Previous
track** past the TempoTec logo until **updater** appears. A valid
`v3_analog_2025.upt` in the card root is flashed without asking, so check which
file is there first. The image only writes the main kernel and rootfs; the
bootloader and the recovery system are never touched.

## Bugs and feedback

Questions, bug reports and ideas go in the Telegram group:
[V3 Blaze Discussion](https://t.me/V3_Blaze_Discussion). When you report a bug,
say which version you run and what you did before it happened.

## Building

Sonix builds as described in [UPSTREAM-README.md](UPSTREAM-README.md)
(`make target` in `sonix-player/`, Rockbox MIPS toolchain). `make target` draws
the icons for the Blaze's 320 px wide panel, so it needs a Python with
`cairosvg` (`ICON_PYTHON=/path/to/python` if `python3` lacks it). The packer builds
the Blaze image from the **official v1.3 firmware** (SHA-256
`5aa1bf262e9241737086076eef0f238e54e75ae226fa0c845d126de11ac01e95`), which is
not redistributed here:

```
sonix-packer/
├── sonix_firmware_packer.sh
├── v3_analog_2025_original.upt   the official v1.3 UPT, renamed
├── sonix_player                  from make target
├── sonix_launch                  from make target
└── assets/BLAZE/                 the Blaze overlay
```

`./sonix_firmware_packer.sh` writes `v3_analog_2025.upt`. It needs
`mksquashfs`, `unsquashfs` and `mkisofs` (or `xorrisofs`).

Streaming services (Tidal, Qobuz, Podcast Index, Last.fm) need keys of your
own, sealed into `assets/BLAZE/usr/resource/sonix/components/streaming-keys.bin`;
see "Streaming keys" in the upstream README.

## License and credits

GPL-3.0, like upstream ([LICENSE](LICENSE)). Sonix Player is the work of
[@Jepl4r](https://github.com/Jepl4r) and its contributors; see the special
thanks at the end of [UPSTREAM-README.md](UPSTREAM-README.md). The Blaze port
is by [@carmine-bin](https://github.com/carmine-bin). Not affiliated with
TempoTec or HiBy.
