#ifndef NAVIDROMEOFFLINE_H
#define NAVIDROMEOFFLINE_H
#include "navidrome.h"

typedef enum { ND_OFF_UNKNOWN, ND_OFF_NONE, ND_OFF_PARTIAL, ND_OFF_COMPLETE } nd_off_state_t;
typedef enum { ND_OFF_TRACK, ND_OFF_ALBUM, ND_OFF_PLAYLIST } nd_off_kind_t;
typedef struct {
	char id[ND_ID_MAX];
	int64_t playlist, revision;
} nd_off_item_t;
typedef struct {
	nd_library_t library;
	nd_off_kind_t kind;
	int count;
	nd_off_item_t items[ND_WINDOW];
} nd_off_request_t;
typedef void (*nd_off_done_cb)(unsigned token, int row, nd_off_state_t state, void *user);
// One active visible window, one coalesced replacement. GUI thread only.
unsigned nd_offline_submit(const nd_off_request_t *request, nd_off_done_cb done, void *user);
void nd_offline_cancel(unsigned token);
bool nd_offline_busy(unsigned token);
void nd_offline_poll(void);		// GUI delivery fallback for a full gui_post queue
void nd_offline_shutdown(void); // outside service mutex; no GUI work
#endif
