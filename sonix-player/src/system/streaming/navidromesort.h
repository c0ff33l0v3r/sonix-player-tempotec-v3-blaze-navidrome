#ifndef NAVIDROMESORT_H
#define NAVIDROMESORT_H

#include "navidromestore.h"
#include "src/system/core/config.h"

static inline nd_order_t nd_sort_valid(nd_kind_t kind, long value) {
	bool valid = value == ND_ORDER_NAME || value == ND_ORDER_NAME_DESC || (kind == ND_ALBUMS && (value == ND_ORDER_YEAR || value == ND_ORDER_YEAR_DESC)) || (kind == ND_TRACKS && value >= ND_ORDER_ALBUM && value <= ND_ORDER_ARTIST_DESC);
	return valid ? (nd_order_t)value : ND_ORDER_NAME;
}

static inline const char *nd_sort_key(nd_kind_t kind, bool favorites) {
	static const char *const keys[] = {"sort_albums", "sort_tracks", "sort_artists", "sort_genres"};
	if (kind < ND_ALBUMS || kind > ND_GENRES || (favorites && kind > ND_TRACKS))
		return NULL;
	return favorites ? (kind == ND_ALBUMS ? "sort_favorite_albums" : "sort_favorite_tracks") : keys[kind];
}

static inline nd_order_t nd_sort_preference(nd_kind_t kind, bool favorites) {
	const char *key = nd_sort_key(kind, favorites);
	return key ? nd_sort_valid(kind, config_get_int("navidrome", key, ND_ORDER_NAME)) : ND_ORDER_NAME;
}

static inline bool nd_sort_save(nd_kind_t kind, bool favorites, nd_order_t order) {
	const char *key = nd_sort_key(kind, favorites);
	if (!key || nd_sort_valid(kind, order) != order)
		return false;
	long previous = config_get_int("navidrome", key, ND_ORDER_NAME);
	config_set_int("navidrome", key, order);
	if (config_get_int("navidrome", key, -1) != (long)order)
		return false;
	if (config_save())
		return true;
	config_set_int("navidrome", key, previous);
	return false;
}

#endif
