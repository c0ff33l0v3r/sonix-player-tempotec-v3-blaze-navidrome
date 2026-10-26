#ifndef NAVIDROMEDOWNLOADSTORE_H
#define NAVIDROMEDOWNLOADSTORE_H
#include "navidromestore.h"
#include <sys/stat.h>

#define ND_DL_PATH 512
#define ND_DL_DEST 256
// Explicit common host/32-bit target limit; finite HTTP also checks LONG_MAX.
#ifndef ND_DL_MAX_BYTES
#define ND_DL_MAX_BYTES INT32_MAX
#endif
typedef enum { NDDL_QUEUED, NDDL_RUNNING, NDDL_VALIDATING, NDDL_INTENT, NDDL_COMPLETE, NDDL_PAUSED, NDDL_FAILED, NDDL_CANCELLED, NDDL_WAITING } nd_dl_state_t;
typedef struct {
	int64_t key, batch;
	nd_playrow_t source;
	char destination[ND_DL_DEST], album_artist[ND_TEXT_MAX];
	char path[ND_DL_PATH], partial[ND_DL_PATH], digest[65], extension[8], error[64];
	int state, multi;
	int64_t bytes, total, mtime, inode;
} nd_dl_job_t;
typedef struct {
	sqlite3 *db;
	char root[ND_DL_PATH], path[1200];
	const char *error;
	bool history; // Additive history marker table; old read-only schema 1 stays readable.
} nd_dl_store_t;
bool nd_dl_paths(const nd_library_t *library, char root[ND_DL_PATH], char path[1200]);
bool nd_dl_open(nd_dl_store_t *store, const nd_library_t *library, bool write);
void nd_dl_close(nd_dl_store_t *store);
bool nd_dl_sql(nd_dl_store_t *store, const char *sql);
bool nd_dl_prepare(nd_dl_store_t *store, sqlite3_stmt **statement, const char *sql);
void nd_dl_bind(sqlite3_stmt *statement, int at, const char *text);
bool nd_dl_insert(nd_dl_store_t *store, int64_t batch, const char *destination, const nd_playrow_t *row, int multi, int *added, int *skipped);
bool nd_dl_get(nd_dl_store_t *store, int64_t key, nd_dl_job_t *job);
bool nd_dl_next(nd_dl_store_t *store, nd_dl_job_t *job);
typedef enum { NDDL_ALL, NDDL_PENDING, NDDL_SUCCESS, NDDL_FAILURE } nd_dl_filter_t;
bool nd_dl_window_filter(nd_dl_store_t *store, int offset, nd_dl_filter_t filter, nd_dl_job_t jobs[ND_WINDOW], int *count, int *total, int counts[9]);
bool nd_dl_window(nd_dl_store_t *store, int offset, nd_dl_job_t jobs[ND_WINDOW], int *count, int *total, int counts[9]);
// Caller owns a write transaction; keeps files, mappings and pending jobs.
bool nd_dl_clear_history(nd_dl_store_t *store);
const char *nd_dl_history_visible(const nd_dl_store_t *store);
bool nd_dl_update(nd_dl_store_t *store, const nd_dl_job_t *job);
bool nd_dl_find(nd_dl_store_t *store, const char *id, nd_dl_job_t *job);
// Opens validated in-card directory components using O_NOFOLLOW. Caller closes.
int nd_dl_directory(const char *root, const char *relative, bool create);
bool nd_dl_destination(const char *relative);
void nd_dl_safe_name(const char *text, const char *fallback, char out[96]);
bool nd_dl_fingerprint(int directory, const char *leaf, char digest[65], struct stat *stat_out, bool (*current)(void *), void *user);
#endif
