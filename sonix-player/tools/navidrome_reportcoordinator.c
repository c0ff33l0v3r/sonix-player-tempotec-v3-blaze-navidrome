#define NAVIDROME_COORDINATOR_MAIN report_baseline_main
#include "navidrome_coordinatorcheck.c"
#include "src/system/streaming/navidromereports.h"
#include "src/system/streaming/navidromereportstore.h"
#include "src/system/streaming/streamturn.h"
#include <netdb.h>

static atomic_bool release_dns;
static atomic_int fail_thread_nth;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *user) {
	if (atomic_load(&fail_thread_nth) && atomic_fetch_sub(&fail_thread_nth, 1) == 1)
		return EAGAIN;
	return __real_pthread_create(thread, attr, entry, user);
}
static atomic_int report_dns_entered, report_dns_finished;
int __real_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int __wrap_getaddrinfo(const char *host, const char *port, const struct addrinfo *hints, struct addrinfo **out) {
	if (!strcmp(host, "report-dns-stall.invalid")) {
		atomic_fetch_add(&report_dns_entered, 1);
		while (!atomic_load(&release_dns))
			usleep(1000);
		atomic_fetch_add(&report_dns_finished, 1);
		return EAI_NONAME;
	}
	return __real_getaddrinfo(host, port, hints, out);
}
static double real_seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static bool before_deadline(void *user) { return real_seconds() < *(double *)user; }
static void dns_check(const char *url) {
	nd_document_t doc;
	int status;
	double deadline = real_seconds() + .1;
	const char *e = nd_api_report("http://report-dns-stall.invalid/base", "fixture user", server_password, "scrobble", "id=test&submission=true", before_deadline, &deadline, &doc, &status);
	assert(e && !strcmp(e, "nd_sync_cancelled"));
	deadline = real_seconds() + 1;
	e = nd_api_report("http://report-dns-stall.invalid/base", "fixture user", server_password, "scrobble", "id=test&submission=true", before_deadline, &deadline, &doc, &status);
	assert(e && atomic_load(&report_dns_entered) == 1); // bounded abandoned helper
	assert(!nd_api_finite_json(url, "fixture user", server_password, "probe", NULL, 1024, NULL, NULL, &doc));
	nd_api_free(&doc); // ordinary audio/sync resolver capacity remains available
	atomic_store(&release_dns, true);
	for (int i = 0; i < 2000 && !atomic_load(&report_dns_finished); i++)
		usleep(1000);
	assert(atomic_load(&report_dns_finished) == 1);
	puts("report DNS cancellation: one independent helper, bounded abandonment and ordinary request progress: passed");
}

static unsigned discontinuity;
bool audio_observe(audio_observation_t *out) {
	*out = (audio_observation_t){.serial = (unsigned)starts, .discontinuity = discontinuity, .status = playback.status, .position = playback.progress_current_secs, .duration = 30, .rate = 1};
	snprintf(out->path, sizeof(out->path), "%s", playback.current_file);
	return true;
}
static unsigned test_ms = 100000;
static void advance(int n, bool advancing) {
	for (int i = 0; i < n; i++) {
		test_ms += 1000;
		nd_reports_test_clock(test_ms);
		if (advancing && playback.status == AUDIO_STATUS_PLAYING)
			playback.progress_current_secs++;
		nd_reports_test_observe();
		drain();
		usleep(110000);
	}
}
static atomic_bool hold_commit, entered_commit;
int __real_sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
int __wrap_sqlite3_exec(sqlite3 *db, const char *sql, int (*cb)(void *, int, char **, char **), void *user, char **error) {
	const char *file = sqlite3_db_filename(db, "main");
	if (file && strstr(file, "/listens-") && !strcmp(sql, "COMMIT") && atomic_load(&hold_commit)) {
		atomic_store(&entered_commit, true);
		while (atomic_load(&hold_commit))
			usleep(1000);
	}
	return __real_sqlite3_exec(db, sql, cb, user, error);
}
int main(int argc, char **argv) {
	if (argc > 1 && !strcmp(argv[1], "--baseline"))
		return report_baseline_main(1, argv);
	assert(argc == 3);
	alarm(40);
	if (!strcmp(argv[2], "dns")) {
		dns_check(argv[1]);
		return 0;
	}
	char dir[] = "/tmp/sonix-report-worker-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save(argv[1], "fixture user", server_password, "vault1"));
	wait_idle();
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	seed(&lib);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&lib);
		if (!lib.loading)
			break;
		usleep(1000);
	}
	assert(lib.ready);
	lv_init();
	lv_display_t *display = lv_display_create(320, 480);
	lv_obj_t *origin = lv_obj_create(NULL);
	lv_screen_load(origin);
	nd_reports_test_clock(test_ms);
	if (!strcmp(argv[2], "undated"))
		nd_reports_test_undated(true);
	if (!strcmp(argv[2], "init-fail"))
		atomic_store(&fail_thread_nth, 3);
	navidromeplay_init();
	atomic_store(&allow_ready, true);
	if (!strcmp(argv[2], "idle")) {
		advance(30, false);
		navidromeplay_shutdown();
		navidrome_reset();
		drain();
		lv_display_delete(display);
		lv_deinit();
		puts("report idle/menu entry: zero network work without history or playback: passed");
		return 0;
	}
	if (!strcmp(argv[2], "offline") || !strcmp(argv[2], "auto-only")) {
		navidrome_test_online(0);
		navidrome_poll_network();
	}
	nd_query_t query = {.kind = ND_TRACKS};
	assert(navidromeplay_select(&lib, &query, "song-00000", origin));
	wait_started(1);
	int prior_stops = stops;
	unsigned prior_popups = fixture_notices;
	if (!strcmp(argv[2], "priority"))
		streamturn_acquire();
	if (!strcmp(argv[2], "store-stall"))
		atomic_store(&hold_commit, true);
	advance(19, true);
	if (!strcmp(argv[2], "undated")) {
		playback.status = AUDIO_STATUS_STOPPED;
		nd_reports_test_observe();
		advance(3, false);
		nd_reports_test_undated(false);
	}
	if (!strcmp(argv[2], "offline") || !strcmp(argv[2], "auto-only")) {
		navidrome_test_online(1);
		advance(8, true);
		navidrome_snapshot_t disconnected;
		navidrome_snapshot(&disconnected);
		assert(disconnected.state == ND_DISCONNECTED);
		if (!strcmp(argv[2], "auto-only")) {
			navidrome_auto_tick(false, true, test_ms);
			navidrome_auto_tick(true, true, test_ms + 1);
			assert(wait_idle().state == ND_CONNECTED);
			nd_report_context_t context;
			assert(navidrome_report_context(&context, NULL) && !context.connected);
		} else {
			assert(navidrome_reconnect());
			wait_idle();
		}
	}
	if (!strcmp(argv[2], "auth-auto")) {
		nd_report_context_t before, after;
		assert(navidrome_report_context(&before, NULL));
		navidrome_test_online(0);
		navidrome_poll_network();
		navidrome_test_online(1);
		navidrome_auto_tick(false, true, test_ms);
		navidrome_auto_tick(true, true, test_ms + 1);
		assert(wait_idle().state == ND_CONNECTED);
		assert(navidrome_report_context(&after, NULL) && !after.connected && after.connection == before.connection);
	}
	if (!strcmp(argv[2], "store-stall")) {
		for (int i = 0; i < 2000 && !atomic_load(&entered_commit); i++)
			usleep(1000);
		assert(atomic_load(&entered_commit));
		// These production service/GUI accesses must finish while COMMIT is frozen.
		navidrome_snapshot_t snapshot;
		navidrome_snapshot(&snapshot);
		assert(snapshot.state == ND_CONNECTED);
		assert(playlist_count() == 17000);
		navidromeplay_tick();
		atomic_store(&hold_commit, false);
	}
	assert(stops == prior_stops && fixture_notices == prior_popups && (playback.status == AUDIO_STATUS_PLAYING || !strcmp(argv[2], "undated")));
	if (!strcmp(argv[2], "priority")) {
		advance(5, true);
		streamturn_release();
	}
	if (!strcmp(argv[2], "preempt")) {
		streamturn_acquire();
		advance(3, true);
		streamturn_release();
	}
	if (!strcmp(argv[2], "lost")) {
		// Only never-attempted history can be delivered after this reconnect.
		advance(12, true);
		assert(navidrome_reconnect());
		wait_idle();
	}
	if (!strcmp(argv[2], "overflow")) {
		streamturn_acquire();
		nd_context_t c;
		assert(navidrome_context(&c, false));
		for (int i = 0; i < 70; i++) {
			starts++;
			playback.progress_current_secs = 0;
			nd_reports_start(&c, &lib, "song-00000", playback.current_file, 30);
			for (int t = 0; t < 16; t++) {
				test_ms += 1000;
				nd_reports_test_clock(test_ms);
				playback.progress_current_secs++;
				nd_reports_test_observe();
			}
		}
		assert(nd_reports_test_drops() > 0);
		streamturn_release();
	}
	advance(18, true);
	navidrome_snapshot_t snapshot;
	navidrome_snapshot(&snapshot);
	assert(snapshot.state == ND_CONNECTED);
	assert(stops == prior_stops && fixture_notices == prior_popups && (playback.status == AUDIO_STATUS_PLAYING || !strcmp(argv[2], "undated")));
	// An actual second source start cannot wait on the reporter's network state.
	int next_start = starts + 1;
	assert(navidromeplay_select(&lib, &query, "song-00001", origin));
	wait_started(next_start);
	assert(playlist_count() == 17000);
	advance(3, true);
	if (!strcmp(argv[2], "lifecycle")) {
		nd_report_context_t old, renewed;
		assert(navidrome_report_context(&old, NULL));
		navidrome_lock();
		assert(!navidrome_report_current(&old, false, false));
		drain();
		navidromeplay_tick();
		assert(navidrome_unlock("vault1"));
		wait_idle();
		assert(navidrome_report_context(&renewed, NULL) && !strcmp(old.scope, renewed.scope));
		assert(navidrome_forget());
		wait_idle();
		assert(!navidrome_report_current(&renewed, false, false));
		assert(navidrome_save(argv[1], "fixture user", server_password, "vault1"));
		wait_idle();
		assert(navidrome_report_context(&renewed, NULL) && strcmp(old.scope, renewed.scope));
		advance(8, false); // old presence/history must not be replayed into new scope
	}
	navidrome_lock();
	drain();
	navidromeplay_tick();
	navidromeplay_shutdown();
	navidrome_reset();
	drain();
	lv_display_delete(display);
	lv_deinit();
	puts("report production coordinator: independent playback/controls, silent failures, exact source and lifecycle: passed");
	return 0;
}
