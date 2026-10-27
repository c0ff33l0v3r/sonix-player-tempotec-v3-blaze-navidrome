#ifndef NAVIDROME_OWNED_FIXTURE_H
#define NAVIDROME_OWNED_FIXTURE_H
#include "src/system/streaming/navidromeowned.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Consumer-boundary fixture: capture/storage is independently exercised by
// navidrome_actioncheck; these rows drive the production destination services.
typedef struct {
	nd_owned_rows_t owned;
	nd_playrow_t samples[3];
	bool fail_read;
} fixture_owned_t;
static unsigned fixture_owned_destroyed;
static int fixture_owned_read(const nd_owned_rows_t *rows, int first, nd_playrow_t *out, int limit) {
	const fixture_owned_t *fixture = (const fixture_owned_t *)rows;
	if (fixture->fail_read)
		return -1;
	int count = first < rows->count ? rows->count - first : 0;
	if (count > limit)
		count = limit;
	for (int i = 0; i < count; i++)
		out[i] = fixture->samples[(first + i) % 3];
	return count;
}
static void fixture_owned_destroy(nd_owned_rows_t *rows) {
	fixture_owned_destroyed++;
	free(rows);
}
static fixture_owned_t *fixture_owned_new(const nd_library_t *library, const nd_context_t *identity, int count) {
	fixture_owned_t *fixture = calloc(1, sizeof(*fixture));
	assert(fixture);
	fixture->owned = (nd_owned_rows_t){.refs = 1, .count = count, .identity = *identity, .library = *library, .read = fixture_owned_read, .destroy = fixture_owned_destroy};
	nd_wipe(fixture->owned.identity.password, sizeof(fixture->owned.identity.password));
	for (int i = 0; i < 3; i++) {
		snprintf(fixture->samples[i].row.id, ND_ID_MAX, "captured-%c", i == 1 ? 'B' : 'A');
		snprintf(fixture->samples[i].row.title, ND_TEXT_MAX, "Captured occurrence %d", i);
		snprintf(fixture->samples[i].album_id, ND_ID_MAX, "captured-album");
	}
	return fixture;
}
#endif
