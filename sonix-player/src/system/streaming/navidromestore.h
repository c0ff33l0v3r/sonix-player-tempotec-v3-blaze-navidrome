#ifndef NAVIDROMESTORE_H
#define NAVIDROMESTORE_H

#include "navidromeapi.h"
#include "src/system/db/sqlite3.h"
#include <stdint.h>
#include <time.h>

#define ND_ID_MAX 256
#define ND_TEXT_MAX 512
#define ND_WINDOW 16
#define ND_LETTER_BUCKETS 28
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
	sqlite3_stmt *insert, *relationship, *genre;
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

typedef struct {
	nd_row_t row;
	char album_id[ND_ID_MAX], album[ND_TEXT_MAX], artist_id[ND_ID_MAX];
} nd_playrow_t;
// Worker-only compact queue index and bounded individual row resolution.
bool nd_store_play_index(nd_store_t *store, int64_t generation, const nd_query_t *query, const char *selected, int64_t **handles, int *count, int *start);
bool nd_store_play_row(nd_store_t *store, int64_t generation, int64_t handle, nd_playrow_t *out);

// Blocking store boundary. Each worker owns its own handle. No GUI SQL calls.
bool nd_store_open(nd_store_t *store, const char *path, const char *scope, bool write, bool rebuild);
void nd_store_close(nd_store_t *store);
bool nd_store_status(nd_store_t *store, nd_library_t *out);
bool nd_store_begin(nd_store_t *store);
struct nd_batch;
bool nd_store_import_batch(nd_store_t *store, const struct nd_batch *batch);
bool nd_store_import(nd_store_t *store, nd_kind_t kind, const nd_document_t *doc, int *count);
bool nd_store_validate(nd_store_t *store);
bool nd_store_publish(nd_store_t *store);
void nd_store_cleanup(nd_store_t *store);
// Logical pins hold no SQLite handle or transaction. At most two generations.
bool nd_store_retain(const char *scope, int64_t generation);
void nd_store_release(const char *scope, int64_t generation);
bool nd_store_query(nd_store_t *store, int64_t generation, const nd_query_t *query, int offset, nd_row_t rows[ND_WINDOW], int *count, int *total);
bool nd_store_letters(nd_store_t *store, int64_t generation, const nd_query_t *query, int first[ND_LETTER_BUCKETS]);
bool nd_store_track(nd_store_t *store, int64_t generation, const char *id, nd_row_t *out);
bool nd_store_contains(nd_store_t *store, int64_t generation, nd_kind_t kind, const char *id);

#endif
