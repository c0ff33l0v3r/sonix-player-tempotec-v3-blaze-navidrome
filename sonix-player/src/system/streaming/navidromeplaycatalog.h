#ifndef NAVIDROMEPLAYCATALOG_H
#define NAVIDROMEPLAYCATALOG_H

#include "navidrome.h"

typedef struct nd_playcatalog nd_playcatalog_t;
typedef struct {
	int64_t playlist, revision, occurrence;
	int ordinal;
	char track[ND_ID_MAX];
	// Zero count retains ordinary-tap compatibility. whole allows worker-chosen starts.
	int count;
	bool whole, single;
} nd_playlist_capture_t;
// Worker-only local revision capture. Repeated track IDs retain distinct positions.
nd_playcatalog_t *nd_playcatalog_playlist(const nd_library_t *library, const nd_context_t *identity,
		const nd_playlist_capture_t *request, bool (*current)(void *), void *user, const char **error);
// Worker-only capture. Remote search preserves server order; album capture uses
// complete disc/track order. No complete library snapshot is published.
nd_playcatalog_t *nd_playcatalog_capture(const nd_library_t *library, const nd_context_t *identity, const nd_query_t *query, const char *selected, bool (*current)(void *), void *user, const char **error);
int nd_playcatalog_count(const nd_playcatalog_t *catalog);
int nd_playcatalog_start(const nd_playcatalog_t *catalog);
bool nd_playcatalog_read(const nd_playcatalog_t *catalog, int index, nd_playrow_t *row);
// Safe on the GUI thread: file cleanup is deferred to a storage worker.
void nd_playcatalog_free(nd_playcatalog_t *catalog);

#endif
