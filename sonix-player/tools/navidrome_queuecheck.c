// Exercise the production queue with an opaque, non-Music source.
#include "src/system/core/config.h"
#include "src/system/playback/playlist.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool fail_allocation;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) {
	if (fail_allocation) {
		fail_allocation = false;
		return NULL;
	}
	return __real_malloc(size);
}

void library_index_close(library_index_t *ix) { assert(!ix); }
bool library_index_stale(const library_index_t *ix) {
	assert(!ix);
	return false;
}
int library_index_count(const library_index_t *ix) {
	assert(!ix);
	return 0;
}
int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	(void)ix;
	(void)offset;
	(void)count;
	(void)cb;
	(void)user;
	assert(!"unexpected Music SQL");
	return 0;
}
library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value, library_order_t order, bool desc) {
	(void)kind;
	(void)filter;
	(void)value;
	(void)order;
	(void)desc;
	assert(!"unexpected Music index");
	return NULL;
}
void library_index_describe(const library_index_t *ix, library_index_spec_t *out) {
	(void)ix;
	memset(out, 0, sizeof(*out));
}
int library_index_find_path(const library_index_t *ix, const char *path) {
	(void)ix;
	(void)path;
	return -1;
}
void library_queue_save_album(const char *album) { (void)album; }

static int closed;
static bool ref(void *unused, size_t entry, char *out, size_t cap) {
	(void)unused;
	return snprintf(out, cap, "ndq:fixture:%zu", entry) < (int)cap;
}
static bool info(void *unused, size_t entry, song_metadata_t *out) {
	(void)unused;
	memset(out, 0, sizeof(*out));
	snprintf(out->title, sizeof(out->title), "Title %zu", entry);
	return true;
}
static void close_source(void *unused) {
	(void)unused;
	closed++;
}

int main(void) {
	playlist_source_t src = {.count = 17000, .path = ref, .metadata = info, .close = close_source};
	char path[512], selected[512];
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	assert(playlist_load_source(&src, 16998));
	assert(playlist_count() == 17000 && playlist_current_index() == 16998);
	fail_allocation = true;
	assert(!playlist_load_source(&src, 0) && playlist_count() == 17000 && playlist_current_index() == 16998 && !closed);
	assert(playlist_current_path(path, sizeof(path)) && !strcmp(path, "ndq:fixture:16998"));
	assert(playlist_advance_auto(path, sizeof(path)) && !strcmp(path, "ndq:fixture:16999"));
	assert(!playlist_advance_auto(path, sizeof(path)));
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ALL);
	assert(playlist_advance_auto(path, sizeof(path)) && !strcmp(path, "ndq:fixture:0"));
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
	assert(playlist_advance_auto(path, sizeof(path)) && !strcmp(path, "ndq:fixture:0"));
	assert(playlist_set_current(7));
	assert(playlist_current_path(selected, sizeof(selected)));
	int remove[] = {0, 1, 5, 7, 16999};
	assert(playlist_remove(remove, 5)); // current slot is preserved
	assert(playlist_count() == 16996 && playlist_has_source());
	assert(playlist_current_path(path, sizeof(path)) && !strcmp(path, selected));
	assert(playlist_move(0, playlist_count() - 1));
	assert(playlist_current_path(path, sizeof(path)) && !strcmp(path, selected));
	assert(playlist_enqueue("/fixture/local.flac"));
	assert(playlist_count() == 16997 && playlist_has_source());
	assert(playlist_next(path, sizeof(path)) && !strcmp(path, "/fixture/local.flac"));
	int local = playlist_current_index();
	assert(playlist_prev(path, sizeof(path)) && !strcmp(path, selected));
	assert(playlist_remove(&local, 1) && playlist_count() == 16996);
	assert(playlist_set_current(0));
	song_metadata_t meta;
	assert(playlist_metadata_at(0, &meta) && meta.title[0]);
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
	int n = playlist_count();
	bool *seen = calloc(17000, sizeof(*seen));
	assert(seen);
	for (int i = 0; i < n; i++) {
		size_t entry = 0;
		assert(playlist_path_at(i, path, sizeof(path)));
		assert(sscanf(path, "ndq:fixture:%zu", &entry) == 1 && entry < 17000 && !seen[entry]);
		seen[entry] = true;
	}
	free(seen);
	playlist_clear();
	assert(closed == 1 && !playlist_has_source());
	src.count = 2;
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	assert(playlist_load_source(&src, 0) && playlist_enqueue("/fixture/local.flac"));
	assert(playlist_next(path, sizeof(path)) && !strcmp(path, "/fixture/local.flac"));
	int streamed[] = {0, 2};
	assert(playlist_remove(streamed, 2) && !playlist_has_source() && closed == 2);
	assert(playlist_current_path(path, sizeof(path)) && !strcmp(path, "/fixture/local.flac"));
	playlist_clear();
	const char *local_list[] = {"/fixture/a.flac", "/fixture/b.mp3"};
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	playlist_load_paths(local_list, 2, 0);
	assert(playlist_next(path, sizeof(path)) && !strcmp(path, local_list[1]));
	playlist_clear();
	// Mixed provider additions preserve current playback and source ownership.
	playlist_load_paths(local_list, 2, 0);
	int before_close = closed;
	assert(playlist_enqueue_source(&src));
	assert(playlist_has_source() && playlist_count() == 4);
	assert(playlist_current_path(path, sizeof(path)) && !strcmp(path, local_list[0]));
	assert(playlist_metadata_at(1, &meta) && !strcmp(meta.title, "Title 0"));
	assert(playlist_enqueue_source(&src));
	assert(playlist_count() == 6 && playlist_path_at(3, path, sizeof(path)) && !strcmp(path, "ndq:fixture:0"));
	assert(playlist_move(2, 5));
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
	assert(playlist_current_path(path, sizeof(path)) && !strcmp(path, local_list[0]));
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	playlist_drop_sources();
	assert(playlist_count() == 2 && !playlist_has_source() && closed == before_close + 2);
	assert(playlist_current_path(path, sizeof(path)) && !strcmp(path, local_list[0]));
	assert(playlist_path_at(1, path, sizeof(path)) && !strcmp(path, local_list[1]));
	playlist_clear();
	before_close = closed;
	assert(playlist_enqueue_source(&src) && playlist_count() == 2 && playlist_current_index() == 0);
	for (int i = 1; i < 8; i++) { src.user = (void *)(intptr_t)i; assert(playlist_enqueue_source(&src)); }
	src.user = (void *)(intptr_t)8;
	int held_count = playlist_count();
	assert(!playlist_enqueue_source(&src) && playlist_count() == held_count);
	playlist_clear();
	assert(closed == before_close + 8);
	playlist_load_paths(local_list, 2, 0);
	fail_allocation = true;
	assert(!playlist_enqueue_source(&src) && playlist_count() == 2 && !playlist_has_source());
	playlist_clear();
	puts("source additions: both mixed directions, ordered batches, shuffle/current preservation, release, eight-owner limit and atomic allocation failure: passed");
	puts("source queue: 17000 entries, modes, edits, sparse local entries, release and path regression passed");
	printf("queue index bookkeeping: %zu bytes/entry on this host; fixed metadata resolution window\n", sizeof(int64_t) + sizeof(uint32_t) + sizeof(size_t));
	return 0;
}
