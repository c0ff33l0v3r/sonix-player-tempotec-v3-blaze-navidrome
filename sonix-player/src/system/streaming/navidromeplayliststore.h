#ifndef NAVIDROMEPLAYLISTSTORE_H
#define NAVIDROMEPLAYLISTSTORE_H
#include "navidromestore.h"
#define ND_PL_NAME 801
#define ND_PL_HASH 65
#define ND_PL_BYTES (2 * 1024 * 1024)
#define ND_PL_TOKENS 131072

typedef struct {
	int64_t id, revision;
	char name[ND_PL_NAME], remote[ND_ID_MAX], baseline[ND_PL_HASH], desired[ND_PL_HASH];
	char changed[128], intent_name[ND_PL_NAME];
	int64_t operation;
	time_t synced;
	int count, pending;
	bool dirty, readonly;
} nd_playlist_t;
typedef struct {
	int64_t occurrence;
	nd_row_t track;
	bool selected;
} nd_playlist_entry_t;
typedef struct {
	sqlite3 *db;
	const char *error;
	char path[1200], scope[33];
	bool leased;
} nd_playlist_store_t;

bool nd_pl_path(const nd_library_t *library, char out[1200]);
bool nd_pl_open(nd_playlist_store_t *s, const char *path, const char *scope, bool write);
void nd_pl_close(nd_playlist_store_t *s);
bool nd_pl_sql(nd_playlist_store_t *s, const char *sql);
bool nd_pl_prepare(nd_playlist_store_t *s, sqlite3_stmt **st, const char *sql);
void nd_pl_bind(sqlite3_stmt *st, int index, const char *value);
bool nd_pl_get(nd_playlist_store_t *s, int64_t id, nd_playlist_t *out);
bool nd_pl_list(nd_playlist_store_t *s, int offset, nd_playlist_t out[ND_WINDOW], int *count, int *total);
bool nd_pl_entries(nd_playlist_store_t *s, int64_t id, int offset, int64_t session, nd_playlist_entry_t out[ND_WINDOW], int *count);
// One coherent read window of an expected revision; first is a dense list offset.
bool nd_pl_play_window(nd_playlist_store_t *s, int64_t id, int64_t revision, int first,
					   nd_playlist_entry_t out[ND_WINDOW], int *count, int *total);
bool nd_pl_play_entry(nd_playlist_store_t *s, int64_t id, int64_t revision, int ordinal,
		int64_t occurrence, const char *track, nd_playlist_entry_t *out, int *total);
bool nd_pl_create(nd_playlist_store_t *s, const char *name, int64_t *id);
bool nd_pl_edit(nd_playlist_store_t *s, int64_t id, int64_t revision, const char *name, bool remove);
bool nd_pl_begin(nd_playlist_store_t *s, int64_t id, int64_t revision);
// Starts a revision-bound transaction; caller commits through its scoped boundary.
// from/to are dense offsets, occurrence/track identify the exact source.
bool nd_pl_move(nd_playlist_store_t *s, int64_t id, int64_t revision,
				int from, int to, int64_t occurrence, const char *track, bool *changed);
bool nd_pl_finish(nd_playlist_store_t *s, int64_t id, bool changed);
bool nd_pl_append(nd_playlist_store_t *s, int64_t id, const nd_row_t *row, bool dedupe, int *added, int *skipped);
bool nd_pl_fingerprint(nd_playlist_store_t *s, int64_t id, char hash[ND_PL_HASH]);
bool nd_pl_copy(nd_playlist_store_t *s, int64_t id, const char *name, int64_t *copy);
bool nd_pl_baseline(nd_playlist_store_t *s, int64_t id, const char *remote, const char *hash, bool readonly, const char *changed);
bool nd_pl_name_valid(const char *name);
// Strict extraction: exact IDs/names, bounded UTF-8 display prefixes, no JSON NULs.
bool nd_pl_field(const json_doc_t *doc, int object, const char *key, char *out, size_t cap, bool required, bool display);
bool nd_pl_decode(nd_playlist_store_t *s, const nd_document_t *doc, int64_t *id, nd_playlist_t *remote);
#endif
