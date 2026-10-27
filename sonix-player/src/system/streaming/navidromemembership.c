#include "navidromemembership.h"
#include "navidromecache.h"
#include "src/system/db/sqlite3.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEMBERS_MEMORY 64
#define MEMBERS_BYTES (4 * 1024 * 1024)
struct nd_album_membership {
	nd_library_t library;
	char album[ND_ID_MAX], path[1200];
	nd_row_t *memory;
	int count;
	unsigned references, cache_epoch, profile_epoch;
	dev_t device;
	ino_t inode;
	struct nd_album_membership *next;
};
static pthread_mutex_t members_lock = PTHREAD_MUTEX_INITIALIZER;
static nd_album_membership_t *members_retired;
static void (*members_wake)(void);

nd_album_membership_t *nd_album_membership_create(const nd_library_t *lib, const char *album, nd_row_t *memory, int count, const char *path, dev_t device, ino_t inode) {
	if (!lib || !album || !*album || strlen(album) >= ND_ID_MAX || count < 0 || (memory ? count > MEMBERS_MEMORY : !path || !*path || strlen(path) >= 1200))
		return NULL;
	nd_album_membership_t *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	nd_context_t context = {0};
	bool valid = navidrome_context(&context, false);
	m->profile_epoch = context.profile_epoch;
	nd_wipe(&context, sizeof(context));
	if (!valid) {
		free(m);
		return NULL;
	}
	m->library = *lib;
	strcpy(m->album, album);
	if (path)
		strcpy(m->path, path);
	m->memory = memory;
	m->count = count;
	m->references = 1;
	m->cache_epoch = nd_cache_epoch();
	m->device = device;
	m->inode = inode;
	return m;
}
nd_album_membership_t *nd_album_membership_retain(nd_album_membership_t *m) {
	if (m) {
		pthread_mutex_lock(&members_lock);
		m->references++;
		pthread_mutex_unlock(&members_lock);
	}
	return m;
}
void nd_album_membership_release(nd_album_membership_t *m) {
	if (!m)
		return;
	pthread_mutex_lock(&members_lock);
	void (*wake)(void) = NULL;
	if (!--m->references) {
		m->next = members_retired;
		members_retired = m;
		wake = members_wake;
	}
	pthread_mutex_unlock(&members_lock);
	if (wake)
		wake();
}
void nd_album_membership_cleanup_waker(void (*wake)(void)) {
	pthread_mutex_lock(&members_lock);
	members_wake = wake;
	bool pending = members_retired != NULL;
	pthread_mutex_unlock(&members_lock);
	if (wake && pending)
		wake();
}
void nd_album_membership_collect(void) {
	pthread_mutex_lock(&members_lock);
	nd_album_membership_t *m = members_retired;
	members_retired = NULL;
	pthread_mutex_unlock(&members_lock);
	while (m) {
		nd_album_membership_t *next = m->next;
		if (*m->path && navidrome_cache_begin(m->library.epoch, m->library.card_epoch)) {
			struct stat st;
			if (!lstat(m->path, &st) && S_ISREG(st.st_mode) && st.st_dev == m->device && st.st_ino == m->inode)
				unlink(m->path);
			navidrome_cache_end();
		}
		free(m->memory);
		free(m);
		m = next;
	}
}
static bool members_current(const nd_album_membership_t *m) {
	nd_context_t context = {0};
	bool valid = navidrome_context(&context, false) && context.profile_epoch == m->profile_epoch && (!*m->path || (context.card_epoch == m->library.card_epoch && nd_cache_io_current(m->cache_epoch)));
	nd_wipe(&context, sizeof(context));
	return valid;
}
bool nd_album_membership_matches(const nd_album_membership_t *m, const nd_library_t *lib, const char *album) { return m && lib && album && !strcmp(m->album, album) && !strcmp(m->library.scope, lib->scope) && (!*m->path || (m->library.epoch == lib->epoch && m->library.card_epoch == lib->card_epoch)) && members_current(m); }
bool nd_album_membership_read(const nd_album_membership_t *m, int offset, nd_row_t rows[ND_WINDOW], int *count, int *total) {
	*count = 0;
	*total = 0;
	if (!m || offset < 0 || offset > m->count || !members_current(m))
		return false;
	*total = m->count;
	if (m->memory) {
		for (int i = offset; i < m->count && *count < ND_WINDOW; i++) {
			if (!*m->memory[i].id || strnlen(m->memory[i].id, ND_ID_MAX) == ND_ID_MAX)
				return false;
			memset(&rows[*count], 0, sizeof(rows[*count]));
			strcpy(rows[(*count)++].id, m->memory[i].id);
		}
		return members_current(m);
	}
	bool io = nd_cache_io_begin(m->cache_epoch);
	bool lease = io && navidrome_cache_begin(m->library.epoch, m->library.card_epoch);
	struct stat st;
	sqlite3 *db = NULL;
	sqlite3_stmt *query = NULL;
	bool ok = lease && !lstat(m->path, &st) && S_ISREG(st.st_mode) && st.st_dev == m->device && st.st_ino == m->inode && st.st_size <= MEMBERS_BYTES && sqlite3_open_v2(m->path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && sqlite3_exec(db, "PRAGMA cache_size=-64", NULL, NULL, NULL) == SQLITE_OK && sqlite3_prepare_v2(db, "SELECT count(*) FROM row", -1, &query, NULL) == SQLITE_OK && sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == m->count;
	sqlite3_finalize(query);
	query = NULL;
	ok = ok && sqlite3_prepare_v2(db, "SELECT id FROM row ORDER BY disc,track,key,id LIMIT ? OFFSET ?", -1, &query, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int(query, 1, ND_WINDOW);
		sqlite3_bind_int(query, 2, offset);
		int rc;
		while ((rc = sqlite3_step(query)) == SQLITE_ROW && *count < ND_WINDOW) {
			const char *id = (const char *)sqlite3_column_text(query, 0);
			int bytes = sqlite3_column_bytes(query, 0);
			if (!id || bytes < 1 || bytes >= ND_ID_MAX || strlen(id) != (size_t)bytes) {
				ok = false;
				break;
			}
			memset(&rows[*count], 0, sizeof(rows[*count]));
			memcpy(rows[(*count)++].id, id, (size_t)bytes + 1);
		}
		ok = ok && rc == SQLITE_DONE && *count == (m->count - offset < ND_WINDOW ? m->count - offset : ND_WINDOW);
	}
	sqlite3_finalize(query);
	if (db)
		sqlite3_close(db);
	if (lease)
		navidrome_cache_end();
	if (io)
		nd_cache_io_end();
	return ok && members_current(m);
}
