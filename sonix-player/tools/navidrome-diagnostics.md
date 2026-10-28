# Navidrome diagnostic capture

Use **Settings → Developer options → Log to microSD**. Navidrome settings
shows this control's location and the actual destination or unavailable status.
There is no additional Navidrome switch. Migration enables the general setting
once if the old Navidrome debug preference was enabled; later OFF is preserved.
A failed configuration save does not claim the migration was persisted.

On the selected card, collect **both** `.local/sonix_player.log` and
`.local/sonix_player.log.old` immediately after reproducing a problem. The shared
writer rotates during recording, limiting each file to 4 MiB. Short captures
preserve useful context. Existing `navidrome-debug-*.log` files are untouched.
An explicit host `SONIX_LOG` path takes precedence and is not rotated. The
existing `run_navidrome_debug.sh` launcher uses that host override.

USB export suspends card capture and releases log handles. Resume starts a new
capture epoch. Diagnostic records queued for a retired capture are discarded,
with loss reported when subsequent traffic permits. Logging enabled with an
unavailable/read-only/full card is different from logging disabled; do not
interpret an enabled preference as proof that bytes reached storage.

## Read the records

Structured lines start with `navidrome-perf v=1`. Other lines are existing shared
player, transport and kernel diagnostics.

| Field | Meaning |
| --- | --- |
| `run`, `capture` | Process run and destination/capture epoch; group by both |
| `op`, `parent` | Locally assigned operation and optional parent; never server IDs |
| `seq`, `mono_us` | Submission sequence and producer monotonic microseconds |
| `component`, `event`, `result` | Fixed vocabulary from `src/system/core/diagnostic.h` |
| `elapsed_us` | Time since the observed operation start; `-1` means unavailable |
| `snapshot=1` | Start was unobserved or predates this capture; do not infer earlier timings |
| `duration_us`, `wait_us`, `commit_us`, `delivery_us` | Measured stage/wait intervals, in microseconds |
| `source`, `kind`, `flags`, `reason` | Numeric classifications specific to component/event |
| `event=loss` | Counts of lock, queue, rate or summary-context losses |
| `event=writer_health` | Queued bytes and last sync duration/error; piggybacks on feature traffic |

An `ok` response means that boundary succeeded, not that playback started. Look
for decoder, PCM and first-write events. A successful publication syscall does
not by itself prove durable registration. `unknown`, `unavailable`, `uncertain`,
`cancelled` and `superseded` must not be counted as successful completion. Missing
terminal records may reflect capture boundaries or overload; absence is not proof
of success. Sequence gaps and loss notes are expected under deliberate saturation.

New records are at most 768 bytes. The typed event budget is 32 KiB/s, with 8 KiB
reserved from ordinary events for terminal/error traffic. Recurring progress is
limited to once per second per operation, using 128 summary slots. The existing
64-KiB queue and 32-KiB held buffer remain bounded. Audio submission uses try-lock
and drops/counts contention rather than waiting for the writer. These are bounds,
not a promise of lossless capture. Existing unstructured output is outside the
new typed-event rate budget.

Playback queue source values are 1=library/Favorites, 2=owned Search/capture,
3=Discovery, 4=saved playlist; 0 means unavailable. Audio intent source is
1=Navidrome coordinator, 0=shared/local or unclassified. A parent of zero can
also reflect session reconstruction or a consumer outside the captured lifetime;
it must not be assigned to whichever track happened to run most recently.

Action kinds are 1=Show Artist, 2=Show Album, 3=Download, 4=favorite. Artist flags
are ready=1, artist ID present=2, artist name present=4. Action rejection reasons
are 1=stale/unavailable target, 2=missing required metadata, 3=browser rejected,
4=invalid destination, 5=service busy/unavailable, 6=retired visit/gesture,
7=allocation failure. Pause reasons are 0=unclassified, 1=user, 2=output removal,
3=sleep, 4=remote. Stop reasons are 0=unclassified, 1=user, 2=USB export, 3=preparation failure.
Do not infer a more specific cause for an unclassified stop.

Publication records include numeric `errno`, filesystem magic, kernel version
packed as `(major << 16) | (minor << 8) | patch`, build 1=host/2=device, and
strategy 1=native atomic no-replace. `syscall=1` means renameat2 was compiled in;
0 means unavailable at compile time. This change does not add an exFAT fallback.
The separate compatibility change must add its own strategy classification when
integrated. File names and directories are not logged.

Sync `prepare_us`, `entity_us`, and `relation_us` are worker aggregates; COMMIT
has its own boundary. Existing transfer/decode and parse sums may overlap between
workers. Compare these with total operation wall time rather than adding them.
List `visible_rows`/`blank_us` are sampled at existing scroll/bind callbacks using
current LVGL geometry. They describe visible track rows, not measured display
frames: an album header alone may legitimately occupy the viewport. Intentional
empty lists use reason 1. Navigation retires the view; hidden time is not a
continued blank interval. No extra layout pass or polling timer is introduced.

## Reproduce the six reported symptoms

Record firmware commit/version, device model, kernel, filesystem, audio format,
sample rate, output route, gapless setting and logging state separately. Use
anonymous case labels, not account, song, artist or playlist names. Keep the same
card, queue, server and output settings between paired runs. For each case run
logging OFF then ON at least three times, alternating order in a second set if
caching/order affects the result. Record sample counts and all unsuccessful runs.

| Case | Repeatable action | Evidence to compare |
| --- | --- | --- |
| Slow sync | Sync the same unchanged library; separate first and warm sync | Wall time, request/parse overlap, admission, SQL/entity/relationship work and COMMIT |
| Late/missing artwork | Next/Previous between known covers, new covers and no-art tracks; include rapid skips | Consumer bind/old-pixels flag, cache miss, priority wait, retry, decode, delivery and displayed/placeholder outcome |
| Cuts or immediate pause | Play the same original locally and through cached/uncached Navidrome at 44.1 and 96 kHz; test explicit pause and jack removal separately | HTTP/decoder/PCM/first-write order, growfile waits, EPIPE/recovery, terminal and pause reason |
| Show Artist | Start from a saved playlist; try Show Artist and Show Album | Source/parent, readiness and ID/name presence; rejection reason. No metadata repair is included here |
| Download rejected on exFAT | Use an empty destination, then a collision fixture containing a disposable test file | Actual syscall errno, strategy, kernel/build/filesystem, flush/validation/publication/registration. Preserve existing files |
| Blank track-list area | Swipe quickly through a large list while playing; repeat while stopped | Requested range, worker/query/delivery/bind timing, sampled row visibility and interval; distinguish header-only viewport |

Capture wall-clock observations for OFF runs using an external stopwatch/video;
do not turn on another internal logger to manufacture an OFF baseline. Compare
median/range or distributions, not one best run. Audio underruns reproducible
only with logging ON, or new ON-only UI stalls, block device acceptance of this
logging change. Host fixtures do not measure Blaze overhead.

## Coverage beyond those symptoms

| Family | Capture actions |
| --- | --- |
| Service/vault/cache | Connect/auto-connect, failed credentials, lock/unlock, protection conversion, usage/clear, Forget/reset using a disposable profile |
| Playback/session | Library, Favorites, playlist, Search and Discovery selection; prefetch, queue additions, rapid skips and restored paused session |
| Browsing | Library/Favorites, playlist detail, Search root/children, Discovery, queue and Download manager windows; leave while work is pending |
| Downloads/offline | Network original, cache copy, pause/resume/cancel, interrupted recovery, offline availability and card removal |
| Favorites/playlists/actions | Single/batch star, captured selections, create/edit/reorder/add/remove/import/Pull/Upload/Publish and uncertain recovery |
| Lyrics | Offer/search/select/save, network/storage failure, cancel or leave before delivery |
| Reports | Listening qualification, queue/send/ack, offline retention, interrupted request and expired/retired event |
| Shared boundaries | GUI mailbox delay, priority wait, standby admission, card/USB transitions |

Keep a case ledger with expected behavior, observed behavior, run/capture IDs,
loss counts, inferred cause and competing explanations. Log rotation, missing
begins or rate loss can make a run inconclusive. Current diagnostics do not fix
sync speed, artwork policy, missing artist IDs, exFAT publication or list rendering.

New typed records contain only fixed labels and numeric values. Controlled
Navidrome HTTP/audio/decode paths exclude credentials, media paths/names, entity
IDs, raw search/SQL, headers and response bodies before enqueueing. The shared
log also contains unrelated legacy/kernel/child output: treat the complete files
as private and review them before sharing. Do not publish real credentials or
runtime card data with reports.

## Focused host verification

From the repository root:

```sh
sh sonix-player/tools/navidrome_debugcheck.sh
python3 sonix-player/tools/navidrome_observabilitycheck.py --output /tmp/navidrome-events.json
```

The first command uses the real writer, small rotation limits and synthetic
faults. The second links a bounded numeric recorder into production-path fixtures
to assert emitted classifications; it does not replace the real writer check.
It also asserts delayed COMMIT/GUI delivery, actual cover display outcomes and
accepted-HTTP parentage followed by decoder failure. Existing LVGL/decoder/SQLite
objects and the prerequisites of each focused runner are required. Neither
command builds the full player or opens an audio output device.

Full host/target builds, production simulator acceptance, MIPS ABI and the paired
Blaze tests remain user-run. A passing host fixture does not establish any of them.
