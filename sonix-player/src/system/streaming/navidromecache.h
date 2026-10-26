#ifndef NAVIDROMECACHE_H
#define NAVIDROMECACHE_H
#include "navidrome.h"
#define ND_AUDIO_BUDGET (1024LL * 1024 * 1024)
#define ND_ART_BUDGET (32ULL * 1024 * 1024)
// Shared publication barrier. No service/card lock is held while draining.
unsigned nd_cache_epoch(void);
bool nd_cache_io_begin(unsigned epoch);
void nd_cache_io_end(void);
bool nd_cache_io_current(unsigned epoch);
bool nd_cache_maintenance_begin(void);
void nd_cache_maintenance_end(void);
unsigned nd_cache_io_count(void);
typedef struct {
	uint64_t audio, art;
	unsigned revision, card_epoch;
	bool busy, clearing, ready;
	const char *error;
} nd_cache_usage_t;
// Worker-owned, fixed-depth traversal; also used by synthetic fixtures.
bool nd_cache_scan(const char *library_path, bool clear, nd_cache_usage_t *usage);
bool nd_cache_usage_request(const nd_library_t *library);
// Caller first stops the Navidrome decoder on the GUI thread. reader_active is worker-safe.
bool nd_cache_clear_request(const nd_library_t *library, bool (*reader_active)(void));
void nd_cache_usage_snapshot(nd_cache_usage_t *usage);
#endif
