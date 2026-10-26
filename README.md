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
Artists includes album artists and track artists with at least one album linked
in the synced library. Contributor credits without a browsable album stay in the
metadata cache but are omitted from the artist list, its count and letter jumps.
This applies to existing snapshots offline, including read-only cards, without
another Sync. Artist names are displayed as supplied; the filter does not repair
damaged tags or hide names merely because they contain `#`.
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
Long-press tracks or albums for Music-style selection, then use **Add to queue**;
row menus also add one exact track or the whole album. Additions honor Music's
**Play next / Add at end** preference and can mix with local Music. Adding to an
empty queue leaves it stopped. Shuffle shows the actual queued titles and
covers; turning it off restores sequential order. Accent lines mark the selected
track or album, including when paused or waiting for an unavailable track.

Skipping while offline selects the intended next track. A cached track plays;
an uncached track remains visible, stopped, with a reconnect message. Another
skip advances from that selection. Reconnect never starts it automatically;
press Play to retry. **Play albums back to back** continues an album queue through
the full Albums order, wrapping at the end; queued additions play first. Natural
Repeat one stays on the current track. All Tracks keeps its complete track queue.
**Open player on tap** controls whether a successful track tap opens Now Playing.

Now Playing's Settings hide local library scans, Library, folder chaining,
track/boot restoration, the local random-entry shuffle preference and the
unavailable Alternative player layout for Navidrome. Global Music settings keep
those controls and their saved values. Shared PCM EQ/PEQ/MSEB, soundfield,
crossfeed, mono/balance, ReplayGain, fades and output settings use the normal audio
path. Native DSD/DoP retain their existing processing bypasses. Gapless still
requires a ready next file and compatible format/output timing; prefetch does
not guarantee a seamless transition. Last.fm behavior is unchanged.

Settings → Power → **Battery usage** lists Navidrome listening time and estimated
battery use over its 24-hour history, including streaming and cached/offline
playback. The initial estimate uses the same uncalibrated playback weight as
Qobuz/Tidal; Sync and background work have no separate attribution. Existing
history is preserved, and older Navidrome listening already recorded as Local
music remains there.

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
Navidrome Settings shows separate **Audio / 1 GiB** and **Artwork / 32 MiB** cache
bars plus their combined size in MiB. **Clear cache** confirms, stops Navidrome playback,
then drains cache work and removes audio/artwork (including owned temporary files)
without reconnecting. The queue stays stopped and retains its track information.
Library/Favorites metadata, playlists, credentials, logs and permanent downloads
are preserved. Partial deletion failures show the remaining usage.

Choose **Download** from a Navidrome track/album menu or long-press selection,
from the alternative album header's Download button,
or from the current Navidrome track's Now Playing breadcrumb menu. Ordinary
Music Now Playing does not offer this action. **Navidrome settings → Downloads**
shows the Download folder and a **Manage downloads** subpage. The manager uses
Queued, Success and Failed tabs with counts and a scrolling track list showing
state, captured path and MiB sizes. Rows show cached/on-demand album covers,
with small placeholders when unavailable. Tap a row for scrollable file/error
details; manager rows do not start playback. Queued includes active/paused jobs; Failed
also shows canceled jobs with their state. The header contains Resume, Pause
and Cancel. Resume in Failed explicitly retries failed jobs; canceled jobs need
a new Download submission. The floating red trash button immediately clears
completed, failed and canceled history across the tabs, without confirmation.
It keeps the files, playback associations and queued/active/paused jobs. Cleared
failures are excluded from Retry failed; submit Download again to retry them.
Deleted/missing files can be downloaded again;
successful Retry restores their playback association. Controls retain completed music. Leaving
the page keeps admitted work running; restart, reconnect, unlock and card return
require an explicit Resume. Interrupted tracks restart from byte zero. Playback
and its lookahead take priority over background downloads.

**Download folder** defaults to `Music/Navidrome` on the selected card; choose
the card root or a visible folder using the shared File Explorer folder picker.
Opening the picker creates the saved destination and any missing parents on a
writable card. Tap the header's New folder icon to name and enter a subfolder,
then confirm **Use this folder** at the bottom. Close or Back leaves the saved
setting unchanged; any empty folders already created remain on the card.
New batches use the new choice; existing jobs and saved files keep their paths.
Files use `Album Artist/Album/Title.ext`, with `Disc N/` for multi-disc
albums. New filenames omit track numbers; existing downloaded files retain their
saved names. Track-number tags remain unchanged. Compilations stay together
under their album credit. Names are made safe for card filesystems and collisions receive identity suffixes; existing
music is never overwritten. Files retain their original bytes, tags and embedded
artwork. Separate artwork/lyrics export and server folder mirroring are excluded.

Complete original cache files can be copied into permanent downloads offline;
active cached playback keeps its original file. Navidrome playback prefers a
verified permanent download before its disposable cache or the server. Missing
or observably changed files lose their association without being deleted.
Server-side replacements under the same ID are not detected automatically.
Files larger than **2 GiB minus one byte** are rejected on host and target.
Publication requires native atomic no-replace rename support from the running
kernel and card filesystem; unsupported storage reports an error with no unsafe
fallback. Actual Blaze/card support still needs device verification.

Downloads are ordinary user-owned music, outside the cache budget. They survive
Forget Server and can be opened through Folders/Music without credentials; that
playback uses local controls, history and battery accounting. Durable download
records live in `.local/navidrome/downloads-<scope>.db`, outside `.cache`.
Folders sees saved files on refresh. Batch completion coalesces normal Music
Detect changes when enabled and initialized, deferring while scanning is busy.
Your selected scan folders and tags still control indexing; include the download
folder in Scan Music if needed. Downloads do not enable detection or run a full
scan. Failed tracks are reported separately from saved files; retry never
replaces successful downloads. Search list download actions remain outside this
feature's scope.

Navidrome settings → **Alternative album view** is on by default and is
independent of Music's setting. Albums open with a large cover, cover-derived
page colors, available artist/genre/year, numbered tracks and multi-disc
headings. The header scrolls away as you browse. Turn it off for the existing
track list with thumbnails. Ordinary album headers offer **Play**, **Shuffle**
and **Queue**; Queue adds one random album track using the usual Play next/Add
at end preference. Header playback shortcuts stay hidden during playlist picking,
track selection and Search; those views keep their own existing track actions.
Older snapshots still open without Sync, omitting optional details they lack.

**Album artwork cache** stores downloaded JPEG/PNG image bytes on the selected
card in `.local/navidrome/.cache/art/<hash>.img`. It is keyed by profile scope,
server cover-art ID, requested pixel size and cache-format version. It is **not
one cache file per song**: tracks/albums sharing a cover ID share the same entry
at a given size. Different requested sizes can produce separate files for that
cover; a new file is written only when artwork is actually downloaded. Sync does
not download artwork in bulk. Visible rows and album/player views request it
on demand.

| View | Requested artwork size | On the 320 px Blaze panel |
| --- | --- | --- |
| Browse, Search, playlist and download rows | `ui_px(72)` square | 48×48 px |
| Alternative album header | `ui_px(300)`, clamped to available width | 200×200 px |
| Now Playing | Current cover-box width, capped at 512 px; Studio uses its own cover width | Normally 320×320 px in the standard player |

`ui_px()` scales reference sizes from the 480 px layout; these are request/display
bounds, not a guarantee that the server's original has that much detail. The
loader validates and decodes the server image on a worker, cropping/scaling it
to the requested square. Decoded images and the player's blurred backdrop live
in bounded RAM slots; there is no separate persistent backdrop or decoded cache
file for every track. Local Music's embedded/folder artwork uses its separate
thumbnail/player cache rather than this Navidrome directory.

An exact-size downloaded response is reused first. Headers can also reuse cached
encodings requested at 480, 320, 300, 240, 200, 72, 64, 48, 43 or 32 px; list rows
check the thumbnail sizes in that set. Actual image dimensions determine whether
another size has enough detail. When only smaller thumbnails exist, an eligible
connected view downloads the requested size. Offline/read-only views, or failed
upgrades, retain the best available smaller copy. Reusing another size does not
write a new cache file. Now Playing keeps its independent exact-size policy.

Artwork has a 32 MiB disk budget with oldest-accessed entries evicted as needed.
Each encoded response is limited to 256 KiB and source dimensions to 512×512;
invalid/missing images use placeholders. Writes publish through a temporary file
after validation. Real download failures have a 30-second cooldown within that
connection; offline reads and repeated opens do not extend it. Explicit Reconnect
allows a new attempt and refreshes an open album header while retaining the old
image until its replacement is ready. A cached thumbnail enlarged offline can
look soft; reconnecting can supply detail that resampling cannot recreate.
Placeholders stay at native size or downscale. Cache keys and diagnostics exclude
passwords, authentication tokens and request URLs.

**Favorites** opens cached **Albums** and **Tracks** lists. Album, track, and artist
favorites are independent: starring an album does not star its tracks. Successful
Add/Remove actions update the corresponding saved list and count automatically;
there is no extra Refresh step for changes made on this player. Use **Refresh**
to discover favorites changed through the server web interface or other devices.
Refresh keeps its own last-successful time and uses the same bounded parallel
fetch pipeline, compressed responses and reused connections as Sync Library.
It may inspect the accessible song catalogue; it downloads no audio or bulk art.

Favorites works without a prior Sync Library. Items saved by this player can be
browsed before the first full Refresh, with guidance that other server favorites
have not been enumerated yet. Saved changes and complete album contents remain
available offline after restart once credentials are available. Newly favorited
albums reuse available metadata or show a loading state while their track details
are fetched. Unavailable details offer **Retry cache update** in the album menu.
A failed/cancelled Refresh preserves saved player changes and the previous base.

Favorite controls follow Music: long-press tracks or albums and use the star-plus
selection button, or use the single **Add to favorites / Remove from favorites**
action in a row's three-dot menu. Album and artist actions favorite that exact
item. Now Playing acts directly on the current track and updates its star silently;
list/selection success uses Music's green check. Writes need available credentials
and an existing connection, and only confirmed items acquire saved favorite state.
Selected-item failures stop further submissions without retrying successful writes.
An uncertain server response is resolved on the next deliberate favorite action.

If the server saves a favorite but card/cache storage fails, the confirmed star
remains and separate cache feedback explains what was not saved locally. Retry
updates only the cache/details; it does not resend the successful favorite request.
Browsing, playback and captured queues remain independent. Artist state is saved
without adding an Artists category. Disposable Favorites data lives in
`.local/navidrome/.cache/favorites.db` alongside other caches.

Show album opens the exact Navidrome album. Show artist opens that primary
artist's local Albums page by ID, including offline; it is unavailable when the
snapshot has no authoritative primary artist ID. Lyrics integration and
listening reports remain deferred.

Navidrome → **Playlists** stores playlists of Navidrome track references on the
selected card. Like Music’s Playlists page, it starts with a **New playlist** row
and shows playlist icons and track counts; **Import from Navidrome** is in the header.
Local copies open and can be renamed or edited offline with available credentials. They
remain available without a library snapshot; adding tracks uses a complete
matching library or Favorites snapshot. Read-only cards permit browsing but cannot save edits.

Use the header download icon or **⋯ → Download playlist** to save all tracks
through the existing download manager. Repeated tracks and pending/verified
downloads are skipped; the playlist keeps its repetitions and current playback
continues. Files use the configured download folder and normal artist/album/disc
layout. No Library Sync is required: missing album details are fetched by exact
track ID when connected. Offline cache promotion requires sufficient saved local
album context. Later playlist edits or Pull do not change an admitted batch.

Track and album menus offer **Add to playlist**. Long press a track or album to
select several, then use the list-plus action. Albums append their individual
tracks in disc/track order; selections follow the displayed list order. Tracks
already present by server ID, including repeats in the same batch, are skipped
with added/skipped counts. Imported repetitions are preserved. In an opened
playlist, long press selects entries for removal; removing an entry does not
delete its audio. The header's list-plus action adds tracks/albums directly to
that playlist. In this Add tracks flow, track taps select entries; album taps
open their tracks for selection. Normal category browsing keeps playback taps.
Rename is in its menu; linked playlists also offer Save as local copy. Saving a
copy asks for its name (the original name is allowed) before creating an unlinked
copy. The collection's row menu can delete only the local copy.

**Upload to Navidrome** creates a server playlist and links the existing local
copy. An imported or uploaded playlist has a header sync icon with **Pull from
Navidrome** (replace the local name and entries) and **Publish to Navidrome**
(replace the linked server name and entries). Local edits never publish
automatically, and Sync Library does not synchronize playlists. Pull confirms
before discarding local edits; Publish confirms replacement and flags a server
version that changed since the last sync. Another server edit can still race
the final check because this API has no conditional update. Readonly/shared
server playlists can be edited locally, but server permissions control Publish.

A write whose outcome is uncertain shows **Needs verification**. Use **Verify
server version** for a known link. If an upload returned no server ID, **Find
uploaded playlist** lists same-named candidates; selecting one verifies its
contents before linking. Creating another playlist instead requires an explicit
confirmation because the earlier upload may already exist. Failed multi-step
updates preserve local edits and the previous successful sync time. No write is
retried automatically. Missing server playlists retain their local copy/link;
Save as local copy creates an unlinked copy for a deliberate new upload.

Playlist definitions live in `.local/navidrome/playlists-<scope>.db`, independently
of replaceable library snapshots. Cache rebuilds do not erase them. Older
binaries ignore this separate database; unsupported/corrupt playlist stores
report an error rather than being reset automatically. Forget/reset retains
its existing explicit scope-removal behavior.

Remote catalogue/content responses are limited to 2 MiB and 131,072 JSON tokens;
encoded mutation forms are limited to 2 MiB, with an 8 MiB explicit response,
parser, and request-buffer budget. Unsupported sizes fail without truncating a
local playlist. Upload requires the server's OpenSubsonic form-POST support.

Playback from saved playlists, audio upload, SD-card file matching, mixed-source
playlists, automatic merge/sync, server deletion, and manual drag reordering remain
outside this feature.

Use the header **Search** button to search Tracks, Albums, or Artists. Enter a
query and press the keyboard's Search key. Connected searches query the server,
even before the first library sync. Disconnected searches use the last complete
matching synced library while credentials are available. A failed server search
falls back to that library when possible, retaining the query and showing the
failure reason. A successful empty server result
stays empty. Reconnection does not replace displayed local results; submit again
to select the current source.

Navidrome can return no server matches for a single letter; try at least two
characters. The page shows a hint for that case. Synced searches still accept
a single character.

Server counts show `+` while more results remain; synced counts are exact.
Offline matching follows Music's case/accent-insensitive name matching, including
literal `%`, `_`, and backslash characters, against metadata retained at the last
sync. Server matching and ordering can differ. Album and artist hits open by ID,
including items absent from the snapshot. Failed child requests can use the exact
matching synced item. Large unpaged children keep the existing response limits;
without writable card storage, only small bounded child lists are available.
Searching and browsing do not publish a library sync or start audio downloads.
Server and synced search share the same interface without source/date sections.

Tap a found track to play it through Now Playing. As in Music search, the queue
contains all matching track results and starts at the tapped track. Selecting a
track inside a found album queues that whole album in disc/track order, including
albums not yet synced. The current Normal, Repeat, Shuffle or Shuffle Repeat
mode is preserved; Shuffle starts with the tapped track and includes the rest of
the captured results. Preparing a server queue may take a moment while its full
metadata is captured. Back, new searches and Sync leave an active queue intact.
Online Artists excludes artists without albums, matching the synced Artists
visibility rule.

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

## Navidrome sync performance

Sync Library still enumerates the full accessible metadata library on explicit
request. It starts with 1,000 items per page, uses up to two ordered page slots,
reuses eligible HTTP/TLS connections, and accepts gzip or identity responses.
A short page ends enumeration; prefetch can issue one extra end-of-list request.
Scan timestamps detect observed server changes during sync; they do not skip
sync or provide a reliable modified-items/deletion feed.

JSON is validated incrementally. Each page has an 8 MiB encoded/16 MiB decoded
response ceiling, a 256 KiB/16,384-token per-entity ceiling, depth 64, and a 1 MiB
arena of extracted metadata. The two slots together cap owned page/parser/HTTP
buffers at 4 MiB; TLS, stacks, SQLite, and the rest of the player use additional
memory. Resource-heavy pages reduce their requested count finitely at the same
offset. A rejected response imports nothing, and failures preserve the previous
complete library. Requests have an eight-second idle timeout and a 30-second
whole-request deadline; Cancel, Lock, Forget, and card export retire pending work.

Focused checks use synthetic credentials and loopback fixtures, without building
or launching the full player:

```sh
sh sonix-player/tools/navidrome_synccheck.sh
NAVIDROME_CHECK_FLAGS=-DHTTP_FINITE_DEADLINE_MS=200 sh sonix-player/tools/navidrome_synccheck.sh --deadline-check
sh sonix-player/tools/navidrome_synccheck.sh --benchmark --baseline --output /tmp/sonix-sync-baseline.json
sh sonix-player/tools/navidrome_synccheck.sh --benchmark --baseline-report /tmp/sonix-sync-baseline.json --output /tmp/sonix-sync-optimized.json
```

The baseline option reads pre-optimization sources from committed revision
`dc74802` into a temporary directory; it does not revert the worktree. Benchmark
mode reports five-run medians and whether the synthetic 3x target is met when a
baseline report is supplied through the commands above. Ordinary checks gate
correctness and resource/request bounds, not noisy wall-clock results. These
host measurements do not establish real-server or Blaze timing. Debug mode adds
redacted phase timings, encoded/decoded bytes, connection reuse, and conservative
buffer allocation high-water diagnostics without collecting server payloads.
Reports keep SQLite allocations, configured worker/resolver stacks, and process
RSS separate; TLS library allocations are identified as uninstrumented. Failed
requests are not replayed automatically; only classified page-resource limits
permit finite count reduction. Existing feedback strings remain unchanged.

## Building

Sonix builds as described in [UPSTREAM-README.md](UPSTREAM-README.md)
(`make target` in `sonix-player/`, Rockbox MIPS toolchain). `make target` draws
the icons for the Blaze's 320 px wide panel, so it needs a Python with
`cairosvg` and `pillow` (`ICON_PYTHON=/path/to/python` if `python3` lacks them).
The same dependencies apply to `make host PANEL_WIDTH=320`. From `sonix-player/`,
an isolated environment can provide them:

```sh
python3 -m venv build_gen/icon-tools
build_gen/icon-tools/bin/python -m pip install cairosvg pillow
make host -j12 PANEL_WIDTH=320 ICON_PYTHON=build_gen/icon-tools/bin/python
```

Wait for `Host build complete` before relaunching the simulator. A failed build
can leave an older executable in place. `make clean` removes this build-tools
environment along with the other generated files.

The packer builds
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
