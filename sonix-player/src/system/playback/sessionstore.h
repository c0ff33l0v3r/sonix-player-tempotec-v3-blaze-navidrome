#ifndef SESSIONSTORE_H
#define SESSIONSTORE_H

#include "src/system/streaming/navidromestore.h"
#include <stddef.h>
#include <stdint.h>

// Worker-only, credential-free durable storage. Runtime providers never enter it.
#define SESSION_FORMAT 1
#define SESSION_BYTES (32 * 1024 * 1024)
typedef enum { SESSION_NONE, SESSION_MUSIC, SESSION_NAVIDROME } session_owner_t;
typedef struct {
	int version, owner, ready, mode, count, selected;
	int64_t activation, revision;
	double position;
	char key[33], card[33], scope[33];
} session_header_t;
typedef struct {
	nd_playrow_t track;
	char local[512]; // Empty for Navidrome; otherwise card-relative, never a URL.
} session_row_t;
typedef struct {
	uint64_t device, inode;
	int64_t bytes, seconds, nanoseconds;
} session_identity_t;
bool session_file_identity(const char *path, session_identity_t *identity);
bool session_file_matches(const char *path, const session_identity_t *identity);
typedef bool (*session_read_cb)(void *user, int canonical, session_row_t *row);
// 1 valid, 0 absent (legacy eligible), -1 invalid/unreadable. Never falls back.
int session_journal_read(const char *path, session_header_t *out);
bool session_journal_write(const char *path, const session_header_t *header);
bool session_random_id(char out[33]);
bool session_card(const char *root, bool create, char out[33]);
bool session_path(const char *root, const char *key, char *out, size_t size);
bool session_payload_write(const char *path, const session_header_t *header, const uint32_t *order, session_read_cb read, void *user);
bool session_payload_open(const char *path, const session_header_t *expected, sqlite3 **db, uint32_t **order);
// renew runs only with every SQLite handle/statement closed, at 128-row boundaries.
bool session_payload_write_scoped(const char *path, const session_header_t *header, const uint32_t *order, session_read_cb read, void *user, bool (*renew)(void *), void *lease);
bool session_payload_open_scoped(const char *path, const session_header_t *expected, sqlite3 **db, uint32_t **order, bool (*renew)(void *), void *lease);
bool session_payload_row(sqlite3 *db, int canonical, session_row_t *row);
bool session_relative_path(const char *path);
// Remove only generated, unreferenced snapshots; account live provider pins.
bool session_payload_budget(const char *root, const char *keep, bool (*pinned)(const char *path), bool reserve);
#endif
