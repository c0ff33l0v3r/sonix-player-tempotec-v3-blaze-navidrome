#ifndef NAVIDROMEOWNED_H
#define NAVIDROMEOWNED_H
#include "navidrome.h"

// Immutable, reference-counted metadata passed between services. Reading and
// final file cleanup are worker operations; retaining/releasing is GUI-safe.
typedef struct nd_owned_rows nd_owned_rows_t;
struct nd_owned_rows {
	unsigned refs;
	int count;
	bool local;			   // Complete matching local source permits structural reuse.
	nd_context_t identity; // No secrets.
	nd_library_t library;
	int (*read)(const nd_owned_rows_t *, int, nd_playrow_t *, int);
	// Worker-only: 0/1 for a proven complete album, -1 for unknown membership.
	int (*album_multi)(const nd_owned_rows_t *, const char *);
	void (*destroy)(nd_owned_rows_t *);
};
static inline nd_owned_rows_t *nd_owned_retain(nd_owned_rows_t *rows) {
	if (rows)
		__sync_add_and_fetch(&rows->refs, 1);
	return rows;
}
static inline void nd_owned_release(nd_owned_rows_t *rows) {
	if (rows && __sync_sub_and_fetch(&rows->refs, 1) == 0)
		rows->destroy(rows);
}
static inline bool nd_owned_current(const nd_owned_rows_t *rows) { return rows && navidrome_playback_current(&rows->identity, false); }
static inline int nd_owned_read(const nd_owned_rows_t *rows, int first, nd_playrow_t *out, int limit) {
	if (!nd_owned_current(rows) || !out || first < 0 || first > rows->count || limit <= 0 || limit > ND_WINDOW)
		return -1;
	int count = rows->read(rows, first, out, limit);
	return count >= 0 && count <= limit && count <= rows->count - first ? count : -1;
}
#endif
