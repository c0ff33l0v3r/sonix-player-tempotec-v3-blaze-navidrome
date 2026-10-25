#ifndef NAVIDROMEPLAY_H
#define NAVIDROMEPLAY_H
#include "navidromeaudio.h"
#include "src/system/library/metadata.h"

void navidromeplay_init(void); // GUI thread, after LVGL initialization
void navidromeplay_shutdown(void);
bool navidromeplay_select(const nd_library_t *library, const nd_query_t *query, const char *id, void *origin);
bool navidromeplay_search(const nd_library_t *library, const nd_context_t *identity, const nd_query_t *query, const char *id, void *origin);
bool navidromeplay_prepare(const char *reference); // true for non-Navidrome references
void navidromeplay_other(const char *path);
void navidromeplay_stop(void);
bool navidromeplay_toggle_pending(bool *playing);
void navidromeplay_tick(void);
bool navidromeplay_next_ready(char *path, size_t cap);
bool navidromeplay_track(const char *path, nd_playrow_t *track, nd_library_t *library);
bool navidromeplay_reference(const char *path);
bool navidromeplay_auto(const char *reference);
bool navidromeplay_preparing(void);
// Shuffle (0), Play (1), or one random source-owned Queue entry (2).
bool navidromeplay_album(const nd_library_t *library, const nd_query_t *query, int action, void *origin);
bool navidromeplay_lookup(const char *reference, nd_playrow_t *row, nd_library_t *library);

#endif
