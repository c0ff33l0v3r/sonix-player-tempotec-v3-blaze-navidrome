#define _GNU_SOURCE
#include "src/system/streaming/navidromereportstore.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void tracking(void) {
	nd_listen_tracker_t t;
	nd_listen_t event = {.profile = 1, .card = 2, .session = 3, .when = 1700000000000LL, .boot = 100}, out;
	snprintf(event.id, sizeof(event.id), "opaque id/&?é");
	audio_observation_t s = {.serial = 1, .status = AUDIO_STATUS_PLAYING, .duration = 240, .rate = 1};
	nd_listen_start(&t, &event, &s, 100, 240);
	for (int i = 1; i < 120; i++) {
		s.position = i;
		assert(!nd_listen_sample(&t, &s, 100 + i, event.when + i * 1000, &out));
	}
	s.position = 120;
	assert(nd_listen_sample(&t, &s, 220, event.when + 120000, &out));
	assert(!strcmp(out.id, event.id) && out.when == event.when && out.session == 3);
	s.position = 121;
	assert(!nd_listen_sample(&t, &s, 221, event.when + 121000, &out));
	for (int scenario = 0; scenario < 8; scenario++) {
		s = (audio_observation_t){.serial = 2, .status = AUDIO_STATUS_PLAYING, .duration = 30, .rate = 1};
		nd_listen_start(&t, &event, &s, 100, 30);
		s.position = 15;
		if (scenario == 0) {
			s.discontinuity++;
			s.position = 200;
		} // seek
		if (scenario == 1)
			s.status = AUDIO_STATUS_PAUSED;
		if (scenario == 2)
			s.position = 0; // buffering
		if (scenario == 3)
			s.serial++; // replacement, including same path
		if (scenario == 4)
			s.duration = 29;
		if (scenario == 5)
			s.duration = 0;
		if (scenario == 6)
			s.position = 0; // seek to zero is not repeat
		assert(!nd_listen_sample(&t, &s, scenario == 7 ? 116 : 101, event.when + 1000, &out));
	}
	s = (audio_observation_t){.serial = 5, .status = AUDIO_STATUS_PLAYING, .rate = 1};
	event.when = 0;
	nd_listen_start(&t, &event, &s, 100, 0);
	for (int i = 1; i <= 15; i++) {
		s.position = i;
		assert(!nd_listen_sample(&t, &s, 100 + i, 0, &out));
	}
	s.duration = 30;
	s.position = 16;
	assert(nd_listen_sample(&t, &s, 116, 1700000016000LL, &out));
	assert(out.when == 1700000000000LL);
	// A pause/resume discontinuity and backward seek preserve qualification.
	s.discontinuity++;
	s.position = 0;
	assert(!nd_listen_sample(&t, &s, 117, 1700000017000LL, &out) && t.qualified);
	// A new same-ID occurrence qualifies independently.
	event.session++;
	event.when = 1700000020000LL;
	s.serial++;
	s.position = 0;
	nd_listen_start(&t, &event, &s, 120, 30);
	for (int i = 1; i <= 15; i++) {
		s.position = i;
		assert(nd_listen_sample(&t, &s, 120 + i, event.when + i * 1000, &out) == (i == 15));
	}
	s = (audio_observation_t){.serial = 9, .status = AUDIO_STATUS_PAUSED, .duration = 30, .rate = 1};
	event.when = 1700000000000LL;
	nd_listen_start(&t, &event, &s, 100, 30);
	assert(!nd_listen_sample(&t, &s, 200, 1700000100000LL, &out));
	s.status = AUDIO_STATUS_PLAYING;
	assert(!nd_listen_sample(&t, &s, 201, 1700000101000LL, &out));
	for (int i = 1; i <= 15; i++) {
		s.position = i;
		assert(nd_listen_sample(&t, &s, 201 + i, 1700000101000LL + i * 1000, &out) == (i == 15));
	}
	assert(out.when == 1700000101000LL); // paused admission is not listening start
	puts("report tracker: threshold, exact identity, repeats, pause/seek/buffering/gaps, late duration/clock: passed");
}
static void storage(void) {
	char dir[] = "/tmp/sonix-report-store-XXXXXX", path[1200];
	assert(mkdtemp(dir));
	snprintf(path, sizeof(path), "%s/.local/navidrome/listens-test.db", dir);
	const char *scope = "0123456789abcdef0123456789abcdef";
	nd_report_store_t s;
	assert(nd_report_store_open(&s, path, scope, true));
	nd_listen_t e = {.when = 1700000000000LL}, got = {0};
	strcpy(e.id, "opaque /? id");
	assert(nd_report_store_put(&s, &e, "one", e.when));
	assert(nd_report_store_put(&s, &e, "one", e.when)); // unique event, no duplicate route
	assert(nd_report_store_take(&s, e.when, &got) > 0 && got.when == e.when && !strcmp(got.id, e.id));
	nd_report_store_close(&s);
	assert(nd_report_store_open(&s, path, scope, false));
	assert(!nd_report_store_take(&s, e.when, &got)); // crash/lost response cannot replay
	for (int i = 0; i < 1030; i++) {
		char key[32];
		snprintf(key, sizeof(key), "event-%d", i);
		e.when++;
		assert(nd_report_store_put(&s, &e, key, e.when));
	}
	sqlite3_stmt *st = NULL;
	assert(sqlite3_prepare_v2(s.db, "SELECT count(*) FROM listens", -1, &st, NULL) == SQLITE_OK);
	assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1024);
	sqlite3_finalize(st);
	// A busy writer cannot make another connection wait or mark/send a row.
	nd_report_store_t other;
	assert(nd_report_store_open(&other, path, scope, false));
	assert(sqlite3_exec(s.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
	assert(!nd_report_store_take(&other, e.when, &got));
	nd_report_store_close(&other);
	assert(sqlite3_exec(s.db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
	assert(!nd_report_store_take(&s, e.when + ND_LISTEN_AGE + 1, &got));
	assert(sqlite3_exec(s.db, "PRAGMA user_version=7", NULL, NULL, NULL) == SQLITE_OK);
	nd_report_store_close(&s);
	assert(!nd_report_store_open(&s, path, scope, false));
	nd_report_store_close(&s);
	struct stat sb;
	assert(!stat(path, &sb) && sb.st_size < 1024 * 1024);
	assert(!chmod(path, 0400));
	assert(!nd_report_store_open(&s, path, scope, false));
	nd_report_store_close(&s);
	char link[1200];
	snprintf(link, sizeof(link), "%s/link", dir);
	assert(!symlink(path, link));
	assert(!nd_report_store_open(&s, link, scope, false));
	nd_report_store_close(&s);
	puts("report store: durable no-replay, dedupe, 1024-row/byte bounds, expiry, busy/read-only/schema/symlink rejection: passed");
}
int main(void) {
	tracking();
	storage();
	return 0;
}
