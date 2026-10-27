#define _GNU_SOURCE 1
#include "src/system/playback/sessionstore.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int boundary = -1, boundary_index, boundary_errno = EIO;
static bool boundary_crash, boundary_after;
static bool inject(int event, bool after) {
	if (event != boundary || after != boundary_after)
		return false;
	if (boundary_crash)
		_exit(91);
	errno = boundary_errno;
	return true;
}
int __real_fsync(int);
int __wrap_fsync(int fd) {
	int event = boundary_index++;
	if (inject(event, false))
		return -1;
	int rc = __real_fsync(fd);
	return inject(event, true) ? -1 : rc;
}
int __real_rename(const char *, const char *);
int __wrap_rename(const char *from, const char *to) {
	int event = boundary_index++;
	if (inject(event, false))
		return -1;
	int rc = __real_rename(from, to);
	return inject(event, true) ? -1 : rc;
}
static bool fail_malloc;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t bytes) {
	if (fail_malloc) {
		fail_malloc = false;
		return NULL;
	}
	return __real_malloc(bytes);
}
static int fail_insert = -1;
int __real_sqlite3_step(sqlite3_stmt *);
int __wrap_sqlite3_step(sqlite3_stmt *st) {
	const char *sql = sqlite3_sql(st);
	if (sql && !strncmp(sql, "INSERT INTO entry", 17) && fail_insert >= 0) {
		if (fail_insert-- == 0)
			return SQLITE_FULL;
	}
	return __real_sqlite3_step(st);
}
static int fail_at = -1;
static int live_databases, cycles, cycle_limit = -1, observed;
int __real_sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int __wrap_sqlite3_open_v2(const char *path, sqlite3 **db, int flags, const char *vfs) {
	int rc = __real_sqlite3_open_v2(path, db, flags, vfs);
	if (rc == SQLITE_OK)
		live_databases++;
	return rc;
}
int __real_sqlite3_close(sqlite3 *);
int __wrap_sqlite3_close(sqlite3 *db) {
	int rc = __real_sqlite3_close(db);
	if (db && rc == SQLITE_OK)
		live_databases--;
	return rc;
}
static bool cycle(void *unused) {
	(void)unused;
	assert(live_databases == 0);
	observed = 0;
	cycles++;
	return cycle_limit < 0 || cycles < cycle_limit;
}

static bool row(void *unused, int i, session_row_t *out) {
	(void)unused;
	if (i == fail_at)
		return false;
	snprintf(out->track.row.id, sizeof(out->track.row.id), "track-%d", i % 3);
	snprintf(out->track.row.title, sizeof(out->track.row.title), "Occurrence %d", i);
	out->track.row.duration = 300;
	snprintf(out->track.album_artist, sizeof(out->track.album_artist), "Album credit %d", i);
	out->track.saved_occurrence = 1000 + i;
	if (i % 7 == 0) {
		snprintf(out->local, sizeof(out->local), "Music/album/%d.flac", i);
		out->track.row.id[0] = 0;
	}
	return true;
}
static bool window_row(void *user, int i, session_row_t *out) {
	assert(++observed <= 128);
	return row(user, i, out);
}
int main(void) {
	char root[] = "/tmp/sonix-sessionstore.XXXXXX";
	assert(mkdtemp(root));
	char journal[1400], payload[1400], other[1400], card[33];
	snprintf(journal, sizeof(journal), "%s/session.db", root);
	session_header_t h = {.version = 1, .owner = SESSION_NAVIDROME, .ready = 1, .mode = 4, .count = 17000, .selected = 16999, .activation = 1, .revision = 7, .position = 83};
	memset(h.scope, 'a', 32);
	h.scope[32] = 0;
	assert(session_journal_read(journal, &h) == 0);
	assert(session_card(root, true, card));
	assert(session_card(root, false, h.card));
	assert(!strcmp(card, h.card));
	assert(session_random_id(h.key));
	session_header_t original = h;
	assert(session_path(root, h.key, payload, sizeof(payload)));
	uint32_t *order = malloc((size_t)h.count * sizeof(*order));
	assert(order);
	for (int i = 0; i < h.count; i++)
		order[i] = (uint32_t)(h.count - 1 - i);
	assert(session_payload_write(payload, &h, order, row, NULL));
	assert(!session_payload_write(payload, &h, order, row, NULL)); // no overwrite
	assert(session_journal_write(journal, &h));
	session_header_t got;
	assert(session_journal_read(journal, &got) == 1);
	assert(got.selected == 16999 && got.position == 83);
	sqlite3 *db = NULL;
	uint32_t *restored = NULL;
	assert(session_payload_open(payload, &got, &db, &restored));
	for (int i = 0; i < h.count; i++)
		assert(order[i] == restored[i]);
	session_row_t r;
	assert(session_payload_row(db, 0, &r));
	assert(r.local[0]);
	assert(session_payload_row(db, 16999, &r));
	assert(!strcmp(r.track.row.id, "track-1"));
	assert(!strcmp(r.track.album_artist, "Album credit 16999") && r.track.saved_occurrence == 17999);
	free(restored);
	sqlite3_close(db);
	// Position/selected slot can advance without copying the immutable payload.
	got.position = 123;
	got.selected = 999;
	assert(session_journal_write(journal, &got));
	assert(session_payload_open(payload, &got, &db, &restored));
	free(restored);
	sqlite3_close(db);
	// Damage, generation mismatch and bad headers never return partial queues.
	got.revision++;
	assert(!session_payload_open(payload, &got, &db, &restored));
	assert(!db && !restored);
	got.revision--;
	got.selected = got.count;
	assert(!session_payload_open(payload, &got, &db, &restored));
	got.selected = 999;
	got.position = NAN;
	assert(!session_journal_write(journal, &got));
	got.position = 123;
	assert(!session_relative_path("../a"));
	assert(!session_relative_path("a/../b"));
	assert(!session_relative_path("a//b"));
	assert(!session_relative_path("/abs"));
	assert(!session_relative_path("ndq:4:1"));
	assert(session_relative_path("Music/こんにちは.flac"));
	// A failing callback and invalid permutation leave no published file.
	assert(session_random_id(got.key));
	assert(session_path(root, got.key, other, sizeof(other)));
	fail_at = 129;
	assert(!session_payload_write(other, &got, order, row, NULL));
	assert(access(other, F_OK));
	fail_at = -1;
	order[2] = order[1];
	assert(!session_payload_write(other, &got, order, row, NULL));
	order[2] = (uint32_t)(h.count - 3);
	// Repeat-heavy 5,000 occurrences with a later selected duplicate.
	got.count = 5000;
	got.selected = 4999;
	got.revision++;
	for (int i = 0; i < got.count; i++)
		order[i] = (uint32_t)i;
	assert(session_payload_write(other, &got, order, row, NULL));
	assert(session_payload_open(other, &got, &db, &restored));
	assert(session_payload_row(db, 4999, &r));
	assert(!strcmp(r.track.row.id, "track-1"));
	free(restored);
	sqlite3_close(db);
	// No clobbering symlinks; read-only opens need no staging writes.
	char linkpath[1500];
	snprintf(linkpath, sizeof(linkpath), "%s/link", root);
	assert(!symlink(payload, linkpath));
	assert(!session_payload_open(linkpath, &h, &db, &restored));
	assert(!session_journal_write(linkpath, &h));
	assert(!chmod(other, 0400));
	assert(session_payload_open(other, &got, &db, &restored));
	free(restored);
	sqlite3_close(db);
	// Pending owner and tombstone are durable and distinct from no-journal migration.
	got.ready = 0;
	got.owner = SESSION_MUSIC;
	got.activation++;
	assert(session_journal_write(journal, &got));
	assert(session_journal_read(journal, &h) == 1);
	assert(!h.ready && h.owner == SESSION_MUSIC);
	got.owner = SESSION_NONE;
	assert(session_journal_write(journal, &got));
	assert(session_journal_read(journal, &h) == 1 && h.owner == SESSION_NONE);
	assert(sqlite3_open_v2(payload, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
	assert(sqlite3_exec(db, "UPDATE entry SET local='../escape' WHERE canonical=0", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
	// Every scoped boundary closes all SQLite handles before dropping its card lease.
	session_header_t window = original;
	assert(session_random_id(window.key));
	char window_path[1400];
	assert(session_path(root, window.key, window_path, sizeof(window_path)));
	for (int i = 0; i < window.count; i++)
		order[i] = (uint32_t)i;
	observed = cycles = 0;
	assert(session_payload_write_scoped(window_path, &window, order, window_row, NULL, cycle, NULL));
	assert(cycles > 100 && live_databases == 0);
	cycles = 0;
	assert(session_payload_open_scoped(window_path, &window, &db, &restored, cycle, NULL));
	assert(cycles > 200);
	free(restored);
	sqlite3_close(db);
	assert(live_databases == 0);
	unlink(window_path);
	assert(session_random_id(window.key));
	assert(session_path(root, window.key, window_path, sizeof(window_path)));
	observed = cycles = 0;
	cycle_limit = 2;
	assert(!session_payload_write_scoped(window_path, &window, order, window_row, NULL, cycle, NULL));
	assert(live_databases == 0);
	// Retired-card cleanup is deferred; a later scoped sweep can remove the orphan.
	unlink(window_path);
	cycle_limit = -1;
	// Allocation and partial SQL/full-storage failures preserve the last journal.
	assert(session_random_id(window.key));
	assert(session_path(root, window.key, window_path, sizeof(window_path)));
	fail_malloc = true;
	assert(!session_payload_write(window_path, &window, order, row, NULL) && access(window_path, F_OK));
	fail_insert = 129;
	assert(!session_payload_write(window_path, &window, order, row, NULL) && access(window_path, F_OK));
	fail_insert = -1;
	// A process ending immediately after the owner fence leaves pending Music,
	// never a mixed queue/position or an inferred fallback to Navidrome.
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		session_header_t fence = original;
		fence.owner = SESSION_MUSIC;
		fence.activation = 90;
		fence.ready = 0;
		_exit(session_journal_write(journal, &fence) ? 0 : 1);
	}
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	session_header_t fenced;
	assert(session_journal_read(journal, &fenced) == 1 && fenced.owner == SESSION_MUSIC && !fenced.ready && fenced.activation == 90);
	// Journal durability boundaries: file sync, atomic publication and directory sync.
	// Both sides of each crash boundary must expose one whole valid generation.
	for (int step = 0; step < 3; step++) {
		for (int after = 0; after < 2; after++) {
			session_header_t before, candidate, visible;
			assert(session_journal_read(journal, &before) == 1);
			candidate = before;
			candidate.revision++;
			child = fork();
			assert(child >= 0);
			if (!child) {
				boundary = step;
				boundary_index = 0;
				boundary_after = after;
				boundary_crash = true;
				session_journal_write(journal, &candidate);
				_exit(99);
			}
			assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 91);
			assert(session_journal_read(journal, &visible) == 1);
			assert(visible.revision == before.revision || visible.revision == candidate.revision);
			assert(visible.owner == before.owner && visible.activation == before.activation && !visible.ready);
		}
		session_header_t before, candidate, visible;
		assert(session_journal_read(journal, &before) == 1);
		candidate = before;
		candidate.revision++;
		boundary = step;
		boundary_index = 0;
		boundary_after = false;
		boundary_crash = false;
		boundary_errno = step == 0 ? ENOSPC : EROFS;
		assert(!session_journal_write(journal, &candidate));
		boundary = -1;
		assert(session_journal_read(journal, &visible) == 1);
		assert(visible.revision == (step < 2 ? before.revision : candidate.revision));
	}
	// Card payload durability failures cannot publish a new journal reference.
	for (int step = 0; step < 2; step++) {
		assert(session_random_id(window.key));
		assert(session_path(root, window.key, window_path, sizeof(window_path)));
		boundary = step;
		boundary_index = 0;
		boundary_after = false;
		boundary_crash = false;
		boundary_errno = step ? EROFS : ENOSPC;
		assert(!session_payload_write(window_path, &window, order, row, NULL));
		boundary = -1;
		assert(session_journal_read(journal, &fenced) == 1 && !fenced.ready && fenced.owner == SESSION_MUSIC);
		unlink(window_path);
	}
	puts("session store: crashes before/after file sync, rename and directory sync; ENOSPC/EROFS fence/payload failures preserve coherent ownership");
	assert(sqlite3_open_v2(journal, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
	assert(sqlite3_exec(db, "UPDATE header SET version=999", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
	assert(session_journal_read(journal, &fenced) == -1 && !session_journal_write(journal, &original));
	assert(sqlite3_open_v2(journal, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
	assert(sqlite3_exec(db, "UPDATE header SET version=1", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
	// All modes retain their exact stored deal; importer behavior is covered by real-queue checks.
	for (int mode = 0; mode < 5; mode++) {
		window.mode = mode;
		window.count = 64;
		window.selected = 31;
		window.position = 29;
		assert(session_random_id(window.key));
		assert(session_path(root, window.key, window_path, sizeof(window_path)));
		for (int i = 0; i < 64; i++)
			order[i] = (uint32_t)(63 - i);
		assert(session_payload_write(window_path, &window, order, row, NULL));
		assert(session_payload_open(window_path, &window, &db, &restored));
		for (int i = 0; i < 64; i++)
			assert(restored[i] == order[i]);
		free(restored);
		sqlite3_close(db);
		if (mode == 4) {
			assert(sqlite3_open_v2(window_path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
			assert(sqlite3_exec(db, "UPDATE entry SET saved_occurrence=-1 WHERE canonical=1", NULL, NULL, NULL) == SQLITE_OK);
			sqlite3_close(db);
			assert(!session_payload_open(window_path, &window, &db, &restored));
			assert(sqlite3_open_v2(window_path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
			assert(sqlite3_exec(db, "ALTER TABLE entry DROP COLUMN album_artist; ALTER TABLE entry DROP COLUMN saved_occurrence", NULL, NULL, NULL) == SQLITE_OK);
			sqlite3_close(db);
			assert(session_payload_open(window_path, &window, &db, &restored));
			assert(session_payload_row(db, 1, &r) && !r.track.album_artist[0] && !r.track.saved_occurrence);
			free(restored);
			sqlite3_close(db);
		}
		unlink(window_path);
	}
	// Use the original manifest to ensure corruption itself is rejected.
	assert(!session_payload_open(payload, &original, &db, &restored));
	// Use the original manifest after tombstone overwritten local header.
	h = got;
	h.owner = SESSION_NAVIDROME;
	h.ready = 1;
	h.count = 17000;
	h.selected = 16999;
	h.activation = 1;
	h.revision = 7;
	// Payload key mismatch also fails closed; malformed paths were exercised separately.
	assert(!session_payload_open(payload, &h, &db, &restored));
	free(order);
	unlink(linkpath);
	unlink(payload);
	unlink(other);
	unlink(journal);
	char journal_lock[1500];
	snprintf(journal_lock, sizeof(journal_lock), "%s.lock", journal);
	unlink(journal_lock);
	snprintf(other, sizeof(other), "%s/.local/navidrome/session-card", root);
	unlink(other);
	snprintf(other, sizeof(other), "%s/.local/navidrome", root);
	rmdir(other);
	snprintf(other, sizeof(other), "%s/.local", root);
	rmdir(other);
	rmdir(root);
	puts("session store: 17,000-entry / 5,000-occurrence round trips, paused offsets, generations, rollback, malformed input, read-only and symlink checks passed");
	return 0;
}
