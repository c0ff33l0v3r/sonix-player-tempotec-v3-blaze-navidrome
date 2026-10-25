#ifndef ND_SEARCH_H
#define ND_SEARCH_H

#include "navidrome.h"

#define ND_SEARCH_QUERY 256
#define ND_SEARCH_CHILD_ROWS 64
#define ND_SEARCH_CHILD_BYTES (4 * 1024 * 1024)
typedef enum { ND_SEARCH_NONE, ND_SEARCH_SERVER, ND_SEARCH_SYNCED } nd_search_source_t;
typedef struct {
	int first, count, total;
	bool more, reset, card_backed;
	nd_kind_t kind;
	nd_search_source_t source;
	int totals[3];
	bool remaining[3];
	nd_row_t rows[ND_WINDOW];
	nd_album_window_t album;
	bool alternate;
	nd_library_t library;
	const char *error, *reason;
} nd_search_result_t;
typedef struct nd_search nd_search_t;
// Worker-only session. Results contain no credential material. Caller supplies
// a cancellation predicate; no GUI or callbacks are invoked by this module.
nd_search_t *nd_search_create(const char *query);
void nd_search_destroy(nd_search_t *search);
void nd_search_cancel_check(nd_search_t *search, bool (*current)(void *), void *user);
bool nd_search_valid(nd_search_t *search);
void nd_search_read(nd_search_t *search, nd_kind_t kind, int first, nd_search_result_t *out);
void nd_search_child(nd_search_t *search, int level, nd_kind_t parent_kind, const char *id, nd_search_result_t *out);
void nd_search_child_read(nd_search_t *search, int level, int first, nd_search_result_t *out);
void nd_search_child_album_read(nd_search_t *search, int level, int offset, int pitch, int disc_pitch, bool alternate, nd_search_result_t *out);
bool nd_search_child_position(nd_search_t *search, int level, const char *id, int pitch, int disc_pitch, bool alternate, int *position);
#endif
