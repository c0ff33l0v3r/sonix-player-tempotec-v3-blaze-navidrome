#ifndef NAVIDROMEPLAY_H
#define NAVIDROMEPLAY_H
#include "navidromeaudio.h"
#include "src/system/library/metadata.h"

// action: 0 shuffle, 1 play in order, 2 enqueue one random track. GUI thread.
bool navidromeplay_album(const nd_library_t *library, const nd_query_t *query, int action, void *origin);
// GUI admission, worker metadata only, atomic source-owned insertion. session=0 uses kind/id.
bool navidromeplay_chain(bool forward); // Compatible manual-boundary entry.
bool navidromeplay_chain_boundary(bool forward, bool automatic);
bool navidromeplay_enqueuing(void);
bool navidromeplay_enqueue(const nd_library_t *library, nd_kind_t kind, const char *id, int64_t session);
bool navidromeplay_matches(const nd_library_t *library, nd_kind_t kind, const char *id);
void navidromeplay_retry_metadata(const char *reference);
bool navidromeplay_lookup(const char *reference, nd_playrow_t *track, nd_library_t *library);
void navidromeplay_init(void); // GUI thread, after LVGL initialization
void navidromeplay_shutdown(void);
bool navidromeplay_select(const nd_library_t *library, const nd_query_t *query, const char *id, void *origin);
bool navidromeplay_search(const nd_library_t *library, const nd_context_t *identity, const nd_query_t *query, const char *id, void *origin);
bool navidromeplay_prepare(const char *reference); // true for non-Navidrome references
void navidromeplay_other(const char *path);
void navidromeplay_stop(void);
void navidromeplay_cache_stop(void);
bool navidromeplay_cache_reader_active(void);
bool navidromeplay_toggle_pending(bool *playing);
void navidromeplay_tick(void);
bool navidromeplay_next_ready(char *path, size_t cap);
bool navidromeplay_track(const char *path, nd_playrow_t *track, nd_library_t *library);
bool navidromeplay_reference(const char *path);
bool navidromeplay_auto(const char *reference);
void navidromeplay_failed_selection(const char *reference);
bool navidromeplay_preparing(void);
#endif
