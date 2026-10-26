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
	bool favorite_known;
	int details;   // 0 complete, 1 loading, 2 unavailable (albums only).
	bool favorite; // Explicit cached annotation, never inherited from album membership.
} nd_row_t;
typedef struct {
	bool ready, writable, syncing, loading;
	bool corrupt;	// Cache format/read failure, independent of last-operation feedback.
	bool favorites; // Independent favorites database/source; counts retain the four-kind ABI.
	unsigned epoch, card_epoch, offer;
	int64_t generation, base;
	time_t finished;
	int counts[4], stage, fetched;
	char path[1024], scope[33];
	const char *error;
} nd_library_t;
typedef struct {
	sqlite3 *db;
	sqlite3_stmt *insert, *relationship, *genre;
	sqlite3_stmt *favorite_album, *favorite_seen, *favorite_member, *favorite_song;
	sqlite3_stmt *local_insert, *local_artist, *annotation;
	bool favorites, local, navigation;
	int64_t view;
	char scope[33], path[1024]; // Exact published source path, including relative roots.
	int64_t generation;
	const char *error;
} nd_store_t;
typedef struct {
	nd_kind_t kind;
	bool favorites;
	// Albums filtered by an artist/genre, or tracks filtered by an album.
	nd_kind_t filter_kind;
	char filter[ND_ID_MAX];
	char search[256]; // Literal substring, separate from an opaque relationship ID.
} nd_query_t;

typedef struct {
	nd_row_t row;
	char album_id[ND_ID_MAX], album[ND_TEXT_MAX], artist_id[ND_ID_MAX];
	char genre[ND_TEXT_MAX]; // Optional display text, never a navigation identity.
	int year;
} nd_playrow_t;

typedef struct {
	nd_playrow_t info;
	int y[ND_WINDOW], ordinal[ND_WINDOW];
	bool heading[ND_WINDOW];
	int extent, offset;
} nd_album_window_t;
bool nd_store_album_position(nd_store_t *store, int64_t generation, const nd_query_t *query,
							 const char *id, int pitch, int disc_pitch, bool alternate, int *position);
// Worker-only windows; pixel pitches come from the GUI, offset excludes the header.
bool nd_store_album_window(nd_store_t *store, int64_t generation, const nd_query_t *query,
						   int offset, int pitch, int disc_pitch, nd_row_t rows[ND_WINDOW],
						   int *count, int *total, nd_album_window_t *layout);
// Worker-only compact queue index and bounded individual row resolution.
bool nd_store_play_index(nd_store_t *store, int64_t generation, const nd_query_t *query, const char *selected, int64_t **handles, int *count, int *start);
bool nd_store_album_beside(nd_store_t *store, int64_t generation, const char *id, bool forward, char next[ND_ID_MAX]);
// Worker-owned TEMP staging; each add deduplicates handles within this batch.
bool nd_store_queue_add(nd_store_t *store, int64_t generation, nd_kind_t kind, const char *id);
bool nd_store_queue_finish(nd_store_t *store, int64_t **handles, int *count);
bool nd_store_play_row(nd_store_t *store, int64_t generation, int64_t handle, nd_playrow_t *out);

// Incremental Favorites transactions. Caller owns the lease/commit barrier.
bool nd_store_local_begin(nd_store_t *store);
bool nd_store_local_put(nd_store_t *store, nd_kind_t kind, const nd_playrow_t *row, bool starred, int details);
bool nd_store_local_activate(nd_store_t *store);
bool nd_store_metadata(nd_store_t *store, int64_t generation, nd_kind_t kind, const char *id, nd_playrow_t *row);
bool nd_store_annotation(nd_store_t *store, int64_t generation, nd_kind_t kind, const char *id, bool *starred);
// Extract one exact bounded metadata object, used by targeted item completion.
bool nd_metadata_row(const json_doc_t *doc, int object, nd_kind_t kind, nd_playrow_t *row);
bool nd_metadata_album_track(const json_doc_t *doc, int object, const char *album, nd_playrow_t *row);

// Validate the independent Subsonic starred timestamp; absence means clear.
bool nd_metadata_starred(const json_doc_t *document, int object, bool *starred);

// Blocking store boundary. Each worker owns its own handle. No GUI SQL calls.
bool nd_store_open(nd_store_t *store, const char *path, const char *scope, bool write, bool rebuild);
bool nd_store_open_source(nd_store_t *store, const char *path, const char *scope, bool write, bool rebuild, bool favorites);
void nd_store_close(nd_store_t *store);
bool nd_store_status(nd_store_t *store, nd_library_t *out);
bool nd_store_begin(nd_store_t *store);
struct nd_batch;
bool nd_store_import_batch(nd_store_t *store, const struct nd_batch *batch);
bool nd_store_import(nd_store_t *store, nd_kind_t kind, const nd_document_t *doc, int *count);
bool nd_store_validate(nd_store_t *store);
bool nd_store_publish(nd_store_t *store);
void nd_store_cleanup(nd_store_t *store);
// Logical pins hold no SQLite handle/transaction. Eight contexts plus staging.
bool nd_store_retain(const char *scope, int64_t generation);
bool nd_store_retain_source(const nd_library_t *library);
void nd_store_release_source(const nd_library_t *library);
void nd_store_release(const char *scope, int64_t generation);
bool nd_store_query(nd_store_t *store, int64_t generation, const nd_query_t *query, int offset, nd_row_t rows[ND_WINDOW], int *count, int *total);
bool nd_store_letters(nd_store_t *store, int64_t generation, const nd_query_t *query, int first[ND_LETTER_BUCKETS]);
bool nd_store_track(nd_store_t *store, int64_t generation, const char *id, nd_row_t *out);
bool nd_store_favorite(nd_store_t *store, int64_t generation, nd_kind_t kind, const char *id);
bool nd_store_contains(nd_store_t *store, int64_t generation, nd_kind_t kind, const char *id);
// Full-library presentation eligibility; contains/metadata retain raw identities.
bool nd_store_artist_browsable(nd_store_t *store, int64_t generation, const char *id);

#endif
