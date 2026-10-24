#ifndef NAVIDROMESTORE_H
#define NAVIDROMESTORE_H

#include "navidromeapi.h"
#include "src/system/db/sqlite3.h"
#include <stdint.h>
#include <time.h>

#define ND_ID_MAX 256
#define ND_TEXT_MAX 512
#define ND_WINDOW 16
typedef enum { ND_ALBUMS, ND_TRACKS, ND_ARTISTS, ND_GENRES } nd_kind_t;
typedef struct {
	char id[ND_ID_MAX], title[ND_TEXT_MAX], artist[ND_TEXT_MAX], cover[ND_ID_MAX];
	int duration, disc, track;
} nd_row_t;
typedef struct {
	bool ready, writable, syncing, loading;
	unsigned epoch, card_epoch, offer;
	int64_t generation;
	time_t finished;
	int counts[4], stage, fetched;
	char path[1024], scope[33];
	const char *error;
} nd_library_t;
typedef struct {
	sqlite3 *db;
	char scope[33];
	int64_t generation;
	const char *error;
} nd_store_t;
typedef struct {
	nd_kind_t kind;
	// Albums filtered by an artist/genre, or tracks filtered by an album.
	nd_kind_t filter_kind;
	char filter[ND_ID_MAX];
} nd_query_t;

// Blocking store boundary. Each worker owns its own handle. No GUI SQL calls.
bool nd_store_open(nd_store_t *store, const char *path, const char *scope, bool write, bool rebuild);
void nd_store_close(nd_store_t *store);
bool nd_store_status(nd_store_t *store, nd_library_t *out);
bool nd_store_begin(nd_store_t *store);
bool nd_store_import(nd_store_t *store, nd_kind_t kind, const nd_document_t *doc, int *count);
bool nd_store_validate(nd_store_t *store);
bool nd_store_publish(nd_store_t *store);
void nd_store_cleanup(nd_store_t *store);
bool nd_store_query(nd_store_t *store, int64_t generation, const nd_query_t *query, int offset, nd_row_t rows[ND_WINDOW], int *count, int *total);
bool nd_store_contains(nd_store_t *store, int64_t generation, nd_kind_t kind, const char *id);

#endif
