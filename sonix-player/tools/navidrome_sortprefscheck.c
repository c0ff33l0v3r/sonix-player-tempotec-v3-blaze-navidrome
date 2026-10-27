#include "src/system/streaming/navidromesort.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
	assert(argc == 2 && getenv("SONIX_CONFIG"));
	const char *path = getenv("SONIX_CONFIG");
	const nd_kind_t kinds[] = {ND_ALBUMS, ND_TRACKS, ND_ARTISTS, ND_GENRES, ND_ALBUMS, ND_TRACKS};
	const nd_order_t orders[] = {ND_ORDER_YEAR_DESC, ND_ORDER_ALBUM_ARTIST_DESC, ND_ORDER_NAME_DESC, ND_ORDER_NAME_DESC, ND_ORDER_YEAR, ND_ORDER_ARTIST_DESC};
	config_init(path);
	if (!strcmp(argv[1], "write")) {
		config_set_int("library", "sort_desc", 7);
		for (int i = 0; i < 6; i++) {
			assert(nd_sort_preference(kinds[i], i >= 4) == ND_ORDER_NAME);
			assert(nd_sort_save(kinds[i], i >= 4, orders[i]));
		}
		assert(config_get_int("library", "sort_desc", 0) == 7);
	} else {
		assert(!strcmp(argv[1], "read"));
		for (int i = 0; i < 6; i++)
			assert(nd_sort_preference(kinds[i], i >= 4) == orders[i]);
		assert(config_get_int("library", "sort_desc", 0) == 7);
		config_set_int("navidrome", "sort_tracks", 999);
		assert(config_save());
		assert(config_init(path));
		assert(nd_sort_preference(ND_TRACKS, false) == ND_ORDER_NAME);
		assert(nd_sort_preference(ND_TRACKS, true) == ND_ORDER_ARTIST_DESC);
		assert(!nd_sort_save(ND_TRACKS, false, ND_ORDER_YEAR));
		char missing[1024];
		snprintf(missing, sizeof(missing), "%s.missing/settings.ini", path);
		config_init(missing);
		assert(!nd_sort_save(ND_TRACKS, false, ND_ORDER_NAME_DESC));
		assert(nd_sort_preference(ND_TRACKS, false) == ND_ORDER_NAME);
		config_init(NULL);
		for (int i = 0; i < 384; i++) {
			char key[24];
			snprintf(key, sizeof(key), "fixture_%d", i);
			config_set_int("fixture", key, i);
		}
		assert(!nd_sort_save(ND_TRACKS, false, ND_ORDER_NAME_DESC));
		assert(nd_sort_preference(ND_TRACKS, false) == ND_ORDER_NAME);
		puts("Six Navidrome preferences across real process restart, Music isolation, invalid values and failed/full-store save: passed");
	}
	return 0;
}
