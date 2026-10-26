#define _GNU_SOURCE
#include "src/system/streaming/navidromedownloadstore.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
	assert(argc == 2);
	nd_library_t lib = {0};
	snprintf(lib.path, sizeof(lib.path), "%s/.local/navidrome/.cache/library.db", argv[1]);
	memset(lib.scope, 'a', 32);
	nd_dl_store_t store;
	assert(nd_dl_open(&store, &lib, true));
	assert(nd_dl_sql(&store, "BEGIN IMMEDIATE; INSERT INTO batch(id,destination,created) VALUES(1,'Music/Navidrome',0)"));
	int added = 0, skipped = 0;
	for (int i = 0; i < 17000; i++) {
		nd_playrow_t row = {0};
		snprintf(row.row.id, sizeof(row.row.id), "id/%d", i);
		snprintf(row.album_id, sizeof(row.album_id), "album/%d", i / 10);
		snprintf(row.row.title, sizeof(row.row.title), "Track %d", i);
		assert(nd_dl_insert(&store, 1, "Music/Navidrome", &row, 0, &added, &skipped));
		if (i == 0)
			assert(nd_dl_insert(&store, 1, "Elsewhere", &row, 0, &added, &skipped));
	}
	assert(added == 17000 && skipped == 1);
	assert(nd_dl_sql(&store, "COMMIT"));
	nd_dl_job_t jobs[ND_WINDOW], job;
	int count, total, counts[9];
	assert(nd_dl_window(&store, 16990, jobs, &count, &total, counts));
	assert(total == 17000 && count == 10 && counts[NDDL_QUEUED] == 17000);
	assert(nd_dl_next(&store, &job) && !strcmp(job.source.row.id, "id/0") && !strcmp(job.destination, "Music/Navidrome"));
	job.state = NDDL_COMPLETE;
	strcpy(job.path, "Music/Navidrome/Artist/Album/01.flac");
	assert(nd_dl_update(&store, &job));
	assert(nd_dl_find(&store, "id/0", &job) && job.state == NDDL_COMPLETE);
	assert(nd_dl_sql(&store, "BEGIN IMMEDIATE; UPDATE job SET state=7; ROLLBACK"));
	assert(nd_dl_get(&store, job.key, &job) && job.state == NDDL_COMPLETE);
	assert(nd_dl_sql(&store, "UPDATE job SET state=6 WHERE key=2; UPDATE job SET state=7 WHERE key=3; UPDATE job SET state=5 WHERE key=4; UPDATE job SET state=1 WHERE key=5; UPDATE job SET state=2 WHERE key=6; UPDATE job SET state=3 WHERE key=7; UPDATE job SET state=8 WHERE key=8"));
	assert(nd_dl_window_filter(&store, 0, NDDL_SUCCESS, jobs, &count, &total, counts));
	assert(total == 1 && count == 1 && jobs[0].state == NDDL_COMPLETE && counts[NDDL_FAILED] == 1);
	assert(nd_dl_window_filter(&store, 0, NDDL_FAILURE, jobs, &count, &total, counts));
	assert(total == 2 && count == 2 && jobs[0].state == NDDL_CANCELLED && jobs[1].state == NDDL_FAILED);
	assert(nd_dl_window_filter(&store, 16990, NDDL_PENDING, jobs, &count, &total, counts));
	assert(total == 16997 && count == 7);
	assert(!nd_dl_window_filter(&store, 0, (nd_dl_filter_t)99, jobs, &count, &total, counts));
	assert(nd_dl_sql(&store, "BEGIN IMMEDIATE"));
	assert(nd_dl_clear_history(&store));
	assert(nd_dl_sql(&store, "ROLLBACK"));
	assert(nd_dl_window_filter(&store, 0, NDDL_SUCCESS, jobs, &count, &total, counts) && total == 1);
	assert(nd_dl_sql(&store, "BEGIN IMMEDIATE"));
	assert(nd_dl_clear_history(&store));
	assert(nd_dl_sql(&store, "COMMIT"));
	assert(nd_dl_window(&store, 0, jobs, &count, &total, counts) && total == 16997 && !counts[NDDL_COMPLETE] && !counts[NDDL_FAILED] && !counts[NDDL_CANCELLED]);
	assert(counts[NDDL_RUNNING] == 1 && counts[NDDL_PAUSED] == 1 && counts[NDDL_INTENT] == 1 && counts[NDDL_VALIDATING] == 1 && counts[NDDL_WAITING] == 1);
	assert(nd_dl_find(&store, "id/0", &job) && job.state == NDDL_COMPLETE);
	// A pre-history version-1 store remains readable without migration.
	assert(nd_dl_sql(&store, "DROP TABLE history_hidden"));
	nd_dl_close(&store);
	assert(nd_dl_open(&store, &lib, false) && !store.history);
	assert(nd_dl_window(&store, 0, jobs, &count, &total, counts) && total == 17000);
	assert(!nd_dl_clear_history(&store));
	nd_dl_close(&store);
	assert(nd_dl_open(&store, &lib, true) && store.history);
	assert(nd_dl_sql(&store, "BEGIN IMMEDIATE") && nd_dl_clear_history(&store) && nd_dl_sql(&store, "COMMIT"));
	nd_dl_close(&store);
	assert(nd_dl_open(&store, &lib, false));
	assert(nd_dl_window(&store, 0, jobs, &count, &total, counts) && total == 16997);
	assert(nd_dl_find(&store, "id/0", &job));
	assert(!nd_dl_sql(&store, "UPDATE job SET state=7"));
	nd_dl_close(&store);
	puts("PASS 17,000-job windows, exact-ID dedupe, rollback and read-only completed lookup");

	nd_library_t wrong = lib;
	memset(wrong.scope, 'b', 32);
	// Same file with another binding must not be adopted.
	char path[1200], root[ND_DL_PATH], alternate[1200];
	assert(nd_dl_paths(&lib, root, path));
	assert(nd_dl_paths(&wrong, root, alternate));
	assert(!link(path, alternate));
	assert(!nd_dl_open(&store, &wrong, true));
	nd_dl_close(&store);
	assert(!unlink(alternate));
	assert(nd_dl_open(&store, &lib, true));
	assert(nd_dl_sql(&store, "PRAGMA user_version=99"));
	nd_dl_close(&store);
	assert(!nd_dl_open(&store, &lib, true));
	nd_dl_close(&store);
	puts("PASS wrong scope and unsupported schema fail without resetting user data");

	assert(nd_dl_destination("") && nd_dl_destination("Music/Navidrome"));
	const char *bad[] = {"../outside", "/absolute", ".local/downloads", "Music/.hidden", "Music//Folder", "Podcast/x", "Audiobooks/x", "Music/x.", "Music/x ", "Music/../x", "Music\\x"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
		assert(!nd_dl_destination(bad[i]));
	int dir = nd_dl_directory(argv[1], "Music/Artist/Album", true);
	assert(dir >= 0);
	close(dir);
	dir = open(argv[1], O_RDONLY | O_DIRECTORY);
	assert(dir >= 0);
	assert(!symlinkat("/tmp", dir, "escape"));
	close(dir);
	assert(nd_dl_directory(argv[1], "escape/child", true) == -1);
	char name[96];
	nd_dl_safe_name("CON", "Unknown", name);
	assert(!strcmp(name, "_CON"));
	nd_dl_safe_name(" ../A:B? ", "Unknown", name);
	assert(!strcmp(name, "AB"));
	nd_dl_safe_name("...", "Unknown", name);
	assert(!strcmp(name, "Unknown"));
	char unicode[101];
	for (int i = 0; i < 25; i++)
		memcpy(unicode + 4 * i, "\xf0\x9f\x8e\xb5", 4);
	unicode[100] = 0;
	nd_dl_safe_name(unicode, "Unknown", name);
	assert(strlen(name) == 80 && !memcmp(name, unicode, 80));
	nd_dl_safe_name("Bad\xc0\xaf\xed\xa0\x80name", "Unknown", name);
	assert(!strcmp(name, "Badname"));
	puts("PASS card-relative path validation, no-follow directories and FAT-safe names");
	puts("PASS transactional history clear, retained mappings/pending states, persistence and additive read-only legacy compatibility");
	puts("4 download-store groups passed");
	return 0;
}
