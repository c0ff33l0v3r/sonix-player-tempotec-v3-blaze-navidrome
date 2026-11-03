# Navidrome artwork scheduling checks

Run from the repository root. These are focused native harnesses, not full player
builds. Use synthetic loopback fixtures and temporary card roots. No real account,
audio output device, power action or production card is used.

## Prerequisites and commands

Use existing `sonix-player/lvgl` sources and host LVGL objects under
`sonix-player/build_host/lvgl`, a C compiler, Pillow, pkg-config, FreeType and SDL2.
`SONIX_LVGL_OBJECTS` can select another existing LVGL object directory for the new
scheduler runner. Do not run a full build just to satisfy missing prerequisites.

```sh
python3 sonix-player/tools/navidrome_artschedulercheck.py --baseline
python3 sonix-player/tools/navidrome_artschedulercheck.py
NAVIDROME_CHECK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie' python3 sonix-player/tools/navidrome_artschedulercheck.py
sh sonix-player/tools/navidrome_artworkcheck.sh
sh sonix-player/tools/navidrome_playerartuicheck.sh
```

Baseline mode extracts artwork and streaming-priority sources from `ef4a4e6`
into a temporary directory without changing the checkout. It expects the exact
cached-player blocking, slot-order priority and unchanged-thumbnail recovery
assertions to fail, and rejects unrelated crashes. It requires that revision in
local Git history. Current mode expects every scenario to pass at 320x480,
480x800 and 480x720. Sanitizer mode instruments compiled application/test sources;
pre-existing LVGL objects retain their original instrumentation.

The scheduler runner uses the real artwork scheduler, HTTP, shared streaming
priority, source cache and decoders. It uses production LVGL thumbnails and
production Now Playing binding/apply/render code; only account/service context
and unrelated player features are fixture-controlled. Explicit held-response,
connection-close and decoder barriers establish ordering. Short polling of
completion flags observes actual events; sleeps do not decide which request wins.
Watchdogs detect stalled tests and are not device latency targets.

## Assertions

- Cached player pixels render before a held unrelated thumbnail response is
  released; both normal/Studio render paths apply cover/backdrop/tint.
- Player work wins ahead of queued rows and can defer an already blocked remote
  row. Shared-source promotion retains one HTTP acquisition after its initiating
  row is released.
- A production visible thumbnail recovers without rebinding after playback
  preemption. Three preemptions produce exactly four attempts, with quiet/backoff
  admission rather than recursive retries.
- Ready local rows progress under sustained player requests. Cached art can load
  while playback demand remains active; background-original demand is distinct
  and cannot replace the report observer.
- Aspect-aware follow-up counts, failed-source cooldown, publication failure
  fallback, offline/read-only/local-only access and worker/allocation failures
  remain bounded.
- HTTP/decode/handoff/withheld-GUI retirement rejects old row/profile/card/Lock/
  cache generations. The service shutdown callback drains active artwork and
  frees candidates/unclaimed images while keeping its fixed workers dormant.
- Wrappers assert one active decoder and acquisition, eventfd idle waits have no
  polling deadline, and repeated acquisitions do not leak descriptors. The two
  worker stacks are fixed at 256 KiB each. Prepared NDPIX1 records share the existing 32-MiB artwork budget.

Existing source-quality/fault tests and player pixel tests provide additional
legacy migration, invalid image, real decoder, geometry, transition and lifecycle
coverage. Their standalone mock boundaries differ from the combined scheduler
runner; a standalone UI pass alone does not prove real-worker recovery.

## Physical acceptance

Full host/MIPS/ABI builds, a production simulator run and Blaze timing remain
separate. On the same card, server, media and output settings, compare baseline
and fixed firmware for cold/warm cache, playback while scrolling, rapid skips,
interruption followed by remaining on the page, and offline browsing. Use at
least five runs per case, retain every outcome and report median/range.

Alternate logging off/on runs and record cover latency, eventual thumbnail
recovery and audible cuts/UI stalls. A reproducible added cut or persistent
placeholder fails acceptance. Host watchdogs and sanitizer success do not establish
Blaze performance, stack headroom, total memory, TLS overhead or audio safety.
Keep complete logs private and credentials out of evidence. ExFAT publication and
the cause of the original audio cuts are outside this fix.

## Follow-up checks

```sh
python3 sonix-player/tools/navidrome_artschedulercheck.py --followup-baseline
python3 sonix-player/tools/navidrome_threadpolicycheck.py --baseline
python3 sonix-player/tools/navidrome_threadpolicycheck.py
python3 sonix-player/tools/navidrome_scrollcheck.py --baseline
python3 sonix-player/tools/navidrome_scrollcheck.py
python3 sonix-player/tools/navidrome_musiccachecheck.py
python3 sonix-player/tools/navidrome_artschedulercheck.py --pcm --mode buffered --mode buffered-preempt
```

The follow-up baseline extracts `a19cd44` and expects repeated warm decoding and
buffered-player admission failures. The scroll baseline expects active-read
supersession. Current scrolling checks cover all three sizes, held reads, flings,
reversal, exact actionable row identity and rejected delivery. Thread-policy
checks execute each extracted production entry-policy call through the real OS
helper on one permitted CPU under contention; they do not execute whole workers.
Missing affinity support is reported rather than counted as success.

The scheduler matrix adds warm zero-decode/zero-HTTP prepared hits, source/render
revision, corrupt/checksummed records, read-only access, failed source and prepared
publication, stale staging eviction and aggregate disk budget. Binding checks use
real workers/LVGL for compatible repaint retention, same-cover action replacement,
independent pixels, descriptor remapping, delayed rebind and Clear no-refill.
Library/Favorites, Search, Discovery and playlist detail now use the same thumbnail
component as Downloads; production page runners exercise their own actions and
navigation. Headers and Now Playing retain specialized bindings.

Music’s isolated cache runner round-trips its real RGB565 cover/backdrop writer
and reader at both widths in normal/Studio, checking independent ownership and
render keys. It does not touch a Music database or test local media extraction.
`NAVIDROME_CHECK_FLAGS` enables ASan/UBSan in these compiled application harnesses.

Headroom policy admits a current player acquisition at 8000 ms, revokes at
4000 ms, and expires after 500 ms without an observation. Unknown timing,
nonmatching/prefetched sources and paused/stale readers remain deferred. Boundary
fixtures cover transfer tokens, hysteresis, urgent commands and expiry. The `--pcm`
probe runs production PCM write/recovery with inert ALSA and controlled sample
progress beside real artwork HTTP; it asserts sample order/count, not hardware
deadlines, decoder estimate accuracy or audible continuity on the Blaze.

Before device comparisons, build the intended target revision, extract the
executable from the actual packaged image and compare its SHA-256 with that
target binary. Check for the new `NDPIX1` format marker as supplemental evidence;
a marker or timestamp alone cannot establish identity. Record the package/binary
hashes and build revision. Run no-playback cold/warm scrolling separately from
active-playback artwork cases, then the five-repetition paired matrix above.
The previous user-built package predates this follow-up and cannot validate it.
