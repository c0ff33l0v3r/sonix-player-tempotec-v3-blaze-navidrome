// Include the production unit to control adversarial admission ordering without
// exposing test-only service APIs. No service thread, socket or runtime data.
#include "src/system/db/sqlite3.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
static atomic_bool entered, release_io, cleanup_done, cleanup_started;
static int fail_commit;
static int delayed_exec(sqlite3 *db, const char *text, int (*cb)(void *, int, char **, char **), void *arg, char **error) {
	if (!strcmp(text, "COMMIT")) {
		atomic_store(&entered, true);
		while (!atomic_load(&release_io))
			usleep(100);
	}
	if (!strcmp(text, "COMMIT") && fail_commit == 1)
		return SQLITE_IOERR;
	int rc = sqlite3_exec(db, text, cb, arg, error);
	return !strcmp(text, "COMMIT") && fail_commit == 2 ? SQLITE_IOERR : rc;
}
#define sqlite3_exec delayed_exec
#include "../src/system/streaming/navidrome.c"
#undef sqlite3_exec

bool gui_post(void (*cb)(void *), void *arg) {
	(void)cb;
	(void)arg;
	return false;
}
static sqlite3 *database;
static nd_context_t context;
static bool action_mode, result;
static bool commit_action(void *db) { return delayed_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK; }
static void *publish(void *unused) {
	(void)unused;
	result = action_mode ? navidrome_playback_action(&context, false, commit_action, database) : navidrome_context_commit(&context, database);
	return NULL;
}
static void *cleanup(void *unused) {
	(void)unused;
	atomic_store(&cleanup_started, true);
	pthread_mutex_lock(&publication);
	atomic_store(&cleanup_done, true);
	pthread_mutex_unlock(&publication);
	return NULL;
}
static void reset(void) {
	memset(&g, 0, sizeof(g));
	g.unlocked = true;
	g.local_epoch = g.card_epoch = g.profile_epoch = 1;
	context = (nd_context_t){.epoch = 1, .card_epoch = 1, .profile_epoch = 1};
	atomic_store(&entered, false);
	atomic_store(&release_io, false);
	assert(sqlite3_exec(database, "BEGIN; INSERT INTO fixture VALUES(1)", NULL, NULL, NULL) == SQLITE_OK);
}
int main(void) {
	assert(sqlite3_open(":memory:", &database) == SQLITE_OK);
	assert(sqlite3_exec(database, "CREATE TABLE fixture(n)", NULL, NULL, NULL) == SQLITE_OK);
	for (action_mode = false;; action_mode = true) {
		for (int retire = 0; retire < 2; retire++) {
			reset();
			pthread_t thread;
			assert(!pthread_create(&thread, NULL, publish, NULL));
			while (!atomic_load(&entered))
				usleep(100);
			// All these calls must finish BEFORE the worker is released.
			navidrome_snapshot_t snapshot;
			nd_library_t library;
			navidrome_snapshot(&snapshot);
			navidrome_library_snapshot(&library);
			assert(navidrome_context_current(&context, false));
			assert(navidrome_playback_current(&context, false));
			assert(!work_activity_suspend_begin());
			pthread_t cleaner;
			if (retire) {
				navidrome_lock();
				assert(!navidrome_context_current(&context, false));
				navidrome_snapshot(&snapshot);
				assert(!snapshot.unlocked);
				atomic_store(&cleanup_done, false);
				atomic_store(&cleanup_started, false);
				assert(!pthread_create(&cleaner, NULL, cleanup, NULL));
				while (!atomic_load(&cleanup_started))
					usleep(100);
				assert(!atomic_load(&cleanup_done));
			}
			assert(!atomic_load(&release_io));
			atomic_store(&release_io, true);
			pthread_join(thread, NULL);
			if (retire) {
				pthread_join(cleaner, NULL);
				assert(atomic_load(&cleanup_done));
			}
			assert(result == !retire);
			assert(!cache_users && !work_activity_busy());
		}
		// Invalidation before admission: the worker cannot even enter COMMIT.
		reset();
		pthread_mutex_lock(&publication);
		pthread_t thread;
		assert(!pthread_create(&thread, NULL, publish, NULL));
		navidrome_lock();
		pthread_mutex_unlock(&publication);
		pthread_join(thread, NULL);
		assert(!result && !atomic_load(&entered));
		assert(sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
		assert(!cache_users && !work_activity_busy());
		for (fail_commit = 1; fail_commit <= 2; fail_commit++) {
			reset();
			atomic_store(&release_io, true);
			publish(NULL);
			assert(!result && !cache_users && !work_activity_busy());
			assert(sqlite3_get_autocommit(database) == (fail_commit == 2));
			if (fail_commit == 1)
				assert(sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
		}
		fail_commit = 0;
		if (action_mode)
			break;
	}
	sqlite3_close(database);
	puts("Production context/download publication: state/context/Lock complete during blocked COMMIT; both invalidation orderings, queued cleanup, failed/uncertain COMMIT and activity release passed.");
}
