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

Enable **Connect automatically** in Navidrome settings once to connect quietly
when you open Navidrome after a reboot. It defaults off, waits for Wi-Fi to be
ready and allows at most one delayed retry after a temporary network failure.
It never turns Wi-Fi on, unlocks encrypted credentials, or connects merely
because the player started. Failures leave offline browsing available; use the
status line's Retry action or settings Reconnect for an explicit attempt.
Automatic connection never offers or starts Sync, refreshes Favorites, reruns a
Search, resumes downloads or starts playback. Listening reports still need a
successful explicit Save & Connect, Reconnect, Update Password or Unlock.

From the main Navidrome settings page, use
**Sync Library** to copy track, album, artist, and genre metadata to the selected
SD card. Connection success and opening settings never show a Sync prompt;
start synchronization only by choosing **Sync Library**. A new sync needs writable
card storage; cancelling or failing a refresh keeps the last successful library.

After syncing, those four categories browse the local index, including offline
once credentials are available (after unlock in encrypted mode). Settings shows
connection status separately from the last sync time and library counts.
List covers remain bound while scrolling past either edge or moving between
overlapping row windows. Headers use the space left by visible controls, and
track rows keep known durations right-aligned beside truncated artist names.
The Cache size card supports swipe-back like the rest of settings; Manage
downloads uses the standard navigation arrow.

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
hides after inactivity. The header Sort button switches Artists and Genres between A–Z and Z–A.
All Tracks and Favorites Tracks offer Title, Album, Album artist and Artist in
both directions, using retained exact-album metadata with unknown values last.
Album/artist groups retain ascending album/disc/track order; their title letter
strip is hidden. Library/Favorites Albums also offer oldest/newest
release year, with unknown years last and no letter strip in year mode.
Library categories and Favorites Tracks/Albums remember six independent choices
across restarts; artist/genre album lists share Library Albums' choice. Album
track lists retain disc/track order. Search keeps its existing ordering.

In a saved playlist, use the **Sort icon after Shuffle** for Original order or either direction
of Title, Album, Album artist or Artist. Each playlist remembers its own view
across restarts, renaming, edits and Pull. Album groups retain ascending disc/track
order; artist groups retain ascending album/disc/track order. Missing optional
fields stay last, using retained metadata or exact-ID local metadata only;
sorting never fetches tags or server metadata. Read-only cards can use previously
saved choices but cannot save a new choice. Legacy entries remain usable with
unknown optional fields. Collection and destination lists stay alphabetical.

Playlist sorting changes the view and new tap/Play/Shuffle captures. It leaves
the saved/server sequence, sync state, existing queue, whole-playlist Add to
queue, Download and Publish unchanged. Original restores the saved sequence;
explicit copies start in Original. Clear cache preserves these preferences.
Ordinary Music sorting remains independent. On the host simulator, set
`SONIX_CONFIG` to retain browse preferences across launches, as with other host
settings. Playlist view preferences live with the saved playlist database.

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
skip advances from that selection. Preparing a track uses inline Now Playing
status and keeps the controls usable without dimming the screen. Existing cover
art stays visible until the next image is ready, with same-cover tracks reusing
it. Reconnect never starts the selected track automatically;
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

Track, album and saved-playlist rows show a small download icon beside their
secondary text. Green means a track is permanently downloaded, or every track
in the album/playlist is downloaded; grey means an album/playlist is partly
downloaded. No icon means none are downloaded or availability is still unknown.
Disposable playback-cache files do not count. Albums need complete known track
lists; saved playlists use their current saved contents, including repeated
tracks, without requiring Library Sync. Library/Favorites, Search and playlist
rows share these rules. Clearing download history or disposable cache preserves
valid permanent-download status. Local checks refresh on download changes and
page entry, and periodically while visible, so externally removed/changed files
lose their indicators after revalidation. Checks do not download music, connect
to the server or change playback.

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

**Album artwork cache** stores one shared encoded JPEG/PNG source per profile,
server cover-art ID and cache-policy version on the selected card in
`.local/navidrome/.cache/art/<hash>.img`. The current format wraps the server
bytes in a small validated header; display size is not part of the source key.
Tracks sharing a cover ID reuse the source across lists, album headers, Queue,
Downloads and Now Playing. Artwork is fetched on visible demand, never in bulk
during Library Sync.

The baseline is **512 pixels on the shorter side**, preserving aspect ratio.
A square source is normally 512x512; portrait/landscape sources can be 512x768
or 768x512. Acquisition requests size 512 first, inspects returned dimensions
and permits one aspect-aware follow-up capped at 2048. Smaller originals and
extreme-aspect images settle on the best bounded response without repeated
upgrade loops. This cannot restore detail absent from the server image.

| View | Locally rendered artwork | On the 320 px Blaze panel |
| --- | --- | --- |
| Browse, Search, playlist and download rows | `ui_px(72)` square | 48x48 px |
| Alternative album header | `ui_px(300)`, clamped to available width | 200x200 px |
| Now Playing | Current cover-box width, capped at 512 px; Studio uses its own cover width | Normally 320x320 px |

Blaze is 320x480, HiBy R3 Pro II 480x720 and R1 480x800; their largest normal
player cover is 480x480. The worker decodes the shared source and derives each
view's existing centered square crop at its display size. Decoded pixels and
player backdrops live in bounded independent RAM slots; no new persistent
per-view derivatives or full-resolution pixel caches are created. Unchanged
bindings retain pixels across redraws, scrolling and track transitions. Local
Music keeps its separate embedded/folder-art cache.

Artwork retains a **32 MiB disk budget**, including old and current formats.
Each new encoded payload is limited to **2 MiB**, source edges to **2048 pixels**,
and source pixel count to **4,194,304**. Existing decode-memory budgets remain.
Failed/interrupted upgrades preserve usable art; invalid/missing art uses the
existing placeholder. Writes publish atomically after validation/decoding.
Network failures have a source-scoped 30-second cooldown within that connection;
offline reads do not extend it. Explicit Reconnect permits eligible retry and
refreshes an open album header while retaining its prior image.

Legacy size-specific files remain readable through finite known-size probes plus
the caller's old exact size. Offline/read-only use needs no migration or write.
Online writable demand upgrades lazily; sufficiently detailed legacy art needs
no HTTP. Settled-source publication can remove matching old variants. Naturally
small or capped responses do not repeatedly upgrade on reconnect. A single
bounded worker-owned encoded source also coalesces adjacent consumers when disk
publication fails. Clear artwork/cache includes both formats.
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
snapshot has no authoritative primary artist ID. Navidrome lyrics use the shared Music view described below.

Navidrome listening reports run silently in the background, independently of
Last.fm. A track at least 30 seconds long counts after you actually listen to
half of it or four minutes, whichever comes first. Pauses, buffering and seek
jumps do not count. Counts use the exact server track identity, including cached
or permanent originals played through Navidrome; opening the same file through
Music remains local playback.

Reports never pause audio, block transport controls, disconnect playback or show
popups/toasts on failure. Audio transfers take priority, so reports can be delayed
or dropped. Qualified offline listens retain their original start times in a
bounded selected-card queue at `.local/navidrome/listens-<scope>.db`, outside
**Clear cache**: up to 1,024 events, 30 days and 2 MiB including storage overhead.
Missing/read-only/full storage uses bounded temporary memory or loses reports.
Delivery needs a valid clock and an already connected profile; startup and Wi-Fi
recovery alone do not reconnect; optional connection on Navidrome entry does
not authorize report delivery or reset a reporting failure circuit. An uncertain or attempted counting report is never
retried automatically, including after restart, so delivery is best effort.

Compatible servers receive current playback state/position with counting disabled;
older servers receive a legacy Now Playing announcement. Past playback states
are never replayed as current activity. Direct player Last.fm settings stay
independent: if Navidrome also forwards to the same external account, choose one
of those routes to avoid external duplicates. No Last.fm login is needed for
Navidrome's own statistics.

Navidrome → **Playlists** stores playlists of Navidrome track references on the
selected card. Like Music’s Playlists page, it starts with a **New playlist** row
and shows playlist icons and track counts; **Import from Navidrome** is in the header.
Local copies open and can be renamed or edited offline with available credentials. They
remain available without a library snapshot; adding tracks from browsing uses a
complete matching library or Favorites snapshot. Now Playing's **Add to playlist**
uses the represented Navidrome track's exact ID and saved metadata, including
Search or saved-playlist tracks absent from Library Sync. The destination picker
retains that track if playback advances while choosing a playlist. Read-only cards
permit browsing but cannot save edits.

Tap a track in an opened playlist to play the complete saved sequence from that
occurrence, preserving repeated tracks and the current repeat/shuffle mode.
Open player on tap controls whether Now Playing opens. Editing, pulling or
deleting the saved playlist later leaves its captured session queue unchanged;
queue edits do not change the saved copy. Playback needs no Library Sync and
uses saved metadata, complete cached audio or an eligible existing connection.
Read-only cards can play captured playlists of up to 64 entries when complete
audio is available; larger lists need writable card storage for a private queue
copy, capped at 32 MiB. Storage limits report failure without shortening the queue.

The detail page's **Play** shortcut starts the complete saved sequence at its
first occurrence in Normal mode. **Shuffle** chooses from the complete sequence
and honors Continuous shuffle. These shortcuts commit their mode with a successful
start; ordinary track taps keep the current mode.

Choose **Add to queue** from a playlist's collection-row or detail menu to queue
its complete saved sequence, or from an entry's menu to queue just that occurrence.
Insertion follows Play next/Add at end, preserves repeated tracks and does not
start audio, change the current mode or edit the saved playlist. No Library Sync
or audio download is needed to queue saved references. Whole-playlist capture has
the same 64-entry read-only limit; one entry from a larger readable playlist needs
no whole-playlist copy. Queue additions keep their captured contents after saved
edits, Pull or deletion.

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

Inside a saved Navidrome playlist, the rearrange button beside Play and Shuffle
turns the row menus into drag grips. Drag a grip to move that occurrence; hold
near either list edge to scroll farther. Dragging elsewhere still scrolls.
Each completed drop saves a local view order; wait for it to finish before another
move. This order survives restart and changes new taps/Play/Shuffle only. It never
changes the saved definition, Local changes status or the order sent by Publish
or Upload. Whole-playlist Add to queue, Download and copies use the saved
sequence. Choose Sort → Original order to clear local dragging and restore that
sequence; entering Rearrange from a field sort restores any retained local drag
order. Pull replaces saved occurrences and clears their local drag positions.
Tap the button again or use Back to leave rearrange mode. Back or interrupted
input cancels an unfinished drag while keeping earlier saved drops.
Rearranging works offline without Library Sync on writable cards, including
local copies of server-readonly playlists. Even explicit Publish excludes local
drag positions. Rearranging never changes an already captured playback queue. Collection/picker name order and
permanent download files are unchanged.

For a locally created unlinked playlist, **Upload to Navidrome** is in its
collection-row and detail three-dot menus, as well as the header sync icon.
It sends the saved name and ordered track references, including empty playlists,
and links the same local copy after verification. It transfers no audio and needs
an existing connection and writable card storage, with no Library Sync required.
An imported or uploaded playlist has a header sync icon with **Pull from
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

Audio upload, SD-card file matching, mixed-source playlists, automatic merge/sync,
and server deletion remain outside this feature.

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

## Navidrome lyrics

Swipe left on Now Playing's cover to open Music's lyrics view, in normal or
Studio layout. Timed lyrics follow the song and support seeking within the
available audio; plain lyrics can be scrolled. The screen stays on while the
lyrics view is visible. This does not enable the Alternative waveform layout.

Previously saved lyrics load locally. With no saved version, opening the lyrics
view requests the exact song's server lyrics using an existing Navidrome
connection. Starting playback with the lyrics hidden, Sync and Reconnect never
fetch lyrics. Advancing while reading requests lyrics for the new visible track.
The search glyph offers **Refresh server lyrics**, **Choose server lyrics** when
several versions exist, and **Search LRCLIB**. LRCLIB searching is explicit and
uses Music's result list, preview and Apply flow; Wi-Fi is sufficient without a
Navidrome connection or Library Sync. In the host simulator, LRCLIB uses the
host's network directly; the device Wi-Fi switch is not required. The Navidrome
source indicator sits in the lower-left corner, clear of the lyrics search control.

If no usable saved or server lyrics are available, the visible lyrics view offers
**Download the lyrics?** once for that track's visit. **Search** explicitly starts
LRCLIB; Cancel leaves the server explanation and search menu available. The
**Looking for the lyrics…** progress card can be dismissed while the search
continues. Device Wi-Fi waiting also shows progress. Closing lyrics or changing
the track retires pending search feedback and prevents late results from opening.
In the results picker, Back or swipe-back from a preview restores the same list
and scroll position; Back from the list returns to Now Playing.

An applied LRCLIB version overrides server lyrics for that exact track. Skipping
while the picker is open keeps its original target. **Use server lyrics** switches
back to a saved server version or fetches a usable replacement before removing
the override. Failure leaves the override intact. Saving a choice requires
writable selected-card storage; a server result that cannot be saved is marked
as available for this session only.

Saved versions live in `.local/navidrome/lyrics-<scope>.db`, independently of
streaming paths and permanent originals. They remain available offline under the
matching accessible profile/card and survive audio/artwork Clear cache. Each
profile admits 1,024 track keys, an 8-MiB main database and 16 MiB including
sidecars; old automatic server copies may be evicted, while applied overrides
are retained. Full storage gives save feedback rather than discarding choices.
Each lyric is limited to 256 KiB and 4,000 lines; server requests admit eight
versions and a 1-MiB complete response. Lyrics are not uploaded to Navidrome or
exported beside permanent downloads. Opening a downloaded file through ordinary
Music continues using its own embedded lyrics and sidecars.
