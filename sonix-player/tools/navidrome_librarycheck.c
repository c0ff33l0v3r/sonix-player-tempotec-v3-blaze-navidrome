// Real HTTP + SQLite + vault/service integration. All data and secrets are fixtures.
#include "src/gui/streaming/navidromeart.h"
#include "src/system/net/http.h"
#include "src/system/streaming/navidrome.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char configuration[1024], endpoint[512];
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}
const char *config_path(void) { return configuration; }
const char *tr(const char *key) { return key; }
bool gui_post(void (*cb)(void *), void *user) {
	cb(user);
	return true;
}
void thread_be_background(const char *name) { (void)name; }

static navidrome_snapshot_t idle(void) {
	navidrome_snapshot_t s;
	for (int i = 0; i < 90000; i++) {
		navidrome_snapshot(&s);
		if (!s.busy)
			return s;
		usleep(1000);
	}
	assert(!"service timeout");
	return s;
}

static void mode(const char *value) {
	char url[700];
	snprintf(url, sizeof(url), "%s/control?mode=%s", endpoint, value);
	char *body = NULL;
	size_t length = 0;
	int status = 0;
	http_req_t request = {.sensitive_url = true, .require_complete_body = true};
	assert(http_request(url, &request, &body, &length, 1024, 8, &status) && status == 200);
	free(body);
	http_clear_request_metadata();
}

static nd_library_t library(void) {
	nd_library_t result;
	navidrome_library_snapshot(&result);
	return result;
}

static nd_library_t sync_ok(int tracks, int albums) {
	assert(navidrome_sync());
	assert(!navidrome_reconnect()); // Conflicting operations cannot enter the worker.
	idle();
	nd_library_t result = library();
	assert(result.ready && !result.syncing && !result.error);
	assert(result.counts[ND_TRACKS] == tracks && result.counts[ND_ALBUMS] == albums);
	return result;
}

static void query_checks(nd_library_t lib) {
	nd_store_t store;
	assert(nd_store_open(&store, lib.path, lib.scope, false, false));
	nd_row_t rows[ND_WINDOW];
	int count, total;
	nd_query_t q = {.kind = ND_TRACKS};
	assert(nd_store_query(&store, lib.generation, &q, 16984, rows, &count, &total));
	assert(total == 17000 && count == ND_WINDOW);
	assert(!strcmp(rows[15].title, "Track 17000"));
	q.kind = ND_ALBUMS;
	assert(nd_store_query(&store, lib.generation, &q, 0, rows, &count, &total));
	assert(total == 1700 && !strcmp(rows[0].id, "album-01699"));
	q.kind = ND_TRACKS;
	q.filter_kind = ND_ALBUMS;
	snprintf(q.filter, sizeof(q.filter), "album-00000");
	assert(nd_store_query(&store, lib.generation, &q, 0, rows, &count, &total));
	assert(total == 10 && count == 10);
	for (int i = 1; i < count; i++)
		assert(rows[i - 1].disc <= rows[i].disc);
	q.kind = ND_ALBUMS;
	q.filter_kind = ND_ARTISTS;
	snprintf(q.filter, sizeof(q.filter), "artist-00");
	assert(nd_store_query(&store, lib.generation, &q, 0, rows, &count, &total));
	assert(total == 85);
	q.filter_kind = ND_GENRES;
	snprintf(q.filter, sizeof(q.filter), "Jazz");
	assert(nd_store_query(&store, lib.generation, &q, 0, rows, &count, &total));
	assert(total == 1700);
	nd_store_close(&store);
}

static cover_image_t art_wait(int slot, bool want) {
	cover_image_t image = {0};
	bool finished = false, found = false;
	for (int i = 0; i < 10000 && !finished; i++) {
		found = navidromeart_take(slot, &image, &finished);
		if (!finished)
			usleep(1000);
	}
	assert(finished && found == want);
	return image;
}

static void artwork_checks(nd_library_t lib) {
	for (int i = 0; i < 2; i++)
		navidromeart_request(i, &lib, "red", 32);
	for (int i = 0; i < 2; i++) {
		cover_image_t image = art_wait(i, true);
		assert(image.pixels[0] == 255 && image.pixels[2] == 0);
		cover_free(&image);
	}
	for (int i = 0; i < 2; i++)
		navidromeart_request(i, &lib, "bad", 32);
	for (int i = 0; i < 2; i++) {
		cover_image_t image = art_wait(i, false);
		cover_free(&image);
	}
	navidromeart_request(0, &lib, "big", 32);
	cover_image_t image = art_wait(0, false);
	cover_free(&image);
	// Add disposable sparse cache fixtures to exercise the actual eviction path.
	char dir[1200];
	snprintf(dir, sizeof(dir), "%s", lib.path);
	char *slash = strrchr(dir, '/');
	assert(slash);
	strcpy(slash, "/art");
	for (int i = 0; i < 34; i++) {
		char path[1300];
		snprintf(path, sizeof(path), "%s/%032x.img", dir, i);
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		assert(fd >= 0);
		assert(!ftruncate(fd, 1024 * 1024));
		close(fd);
	}
	navidromeart_request(0, &lib, "blue", 32);
	image = art_wait(0, true);
	assert(image.pixels[2] == 255);
	cover_free(&image);
	DIR *d = opendir(dir);
	assert(d);
	struct dirent *entry;
	size_t used = 0;
	while ((entry = readdir(d))) {
		if (entry->d_name[0] == '.')
			continue;
		char path[1500];
		snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
		struct stat st;
		assert(!stat(path, &st));
		used += st.st_size;
	}
	closedir(d);
	assert(used <= 32 * 1024 * 1024);
	// Rebinding while a slower image is in flight may only deliver the new key.
	navidromeart_request(0, &lib, "slow", 32);
	usleep(10000);
	navidromeart_request(0, &lib, "blue", 32);
	image = art_wait(0, true);
	assert(image.pixels[2] == 255);
	cover_free(&image);
	for (int i = 0; i < ND_WINDOW; i++)
		navidromeart_release(i);
	puts("artwork decode/cache reuse/coalescing/cooldown/budget/stale rows: passed");
}

static void disk_full_check(nd_library_t previous) {
	nd_store_t store;
	assert(nd_store_open(&store, previous.path, previous.scope, true, false));
	assert(nd_store_begin(&store));
	sqlite3_stmt *st;
	assert(sqlite3_prepare_v2(store.db, "PRAGMA page_count", -1, &st, NULL) == SQLITE_OK);
	assert(sqlite3_step(st) == SQLITE_ROW);
	int pages = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	char pragma[128];
	snprintf(pragma, sizeof(pragma), "PRAGMA max_page_count=%d", pages);
	assert(sqlite3_exec(store.db, pragma, NULL, NULL, NULL) == SQLITE_OK);
	bool failed = false;
	for (int batch = 0; batch < 4096 && !failed; batch++) {
		nd_document_t doc = {0};
		doc.body = malloc(40000);
		assert(doc.body);
		int used = snprintf(doc.body, 40000, "{\"subsonic-response\":{\"searchResult3\":{\"artist\":[");
		char name[481];
		memset(name, 'x', 480);
		name[480] = 0;
		for (int i = 0; i < 50; i++)
			used += snprintf(doc.body + used, 40000 - used, "%s{\"id\":\"space-%d-%d\",\"name\":\"%s\"}", i ? "," : "", batch, i, name);
		snprintf(doc.body + used, 40000 - used, "]}}}");
		assert(json_parse_limited(doc.body, &doc.doc, ND_API_TOKENS));
		doc.response = json_get(&doc.doc, json_root(&doc.doc), "subsonic-response");
		int count;
		failed = !nd_store_import(&store, ND_ARTISTS, &doc, &count);
		nd_api_free(&doc);
	}
	assert(failed && store.error && !strcmp(store.error, "nd_cache_error"));
	nd_library_t after = {0};
	assert(nd_store_status(&store, &after));
	assert(after.ready && after.generation == previous.generation && after.counts[ND_TRACKS] == 17000);
	sqlite3_exec(store.db, "PRAGMA max_page_count=1073741823", NULL, NULL, NULL);
	nd_store_cleanup(&store);
	nd_store_close(&store);
	puts("SQLite disk-full injection preserves published snapshot: passed");
}

static void rejected(const char *fixture, const char *expected, nd_library_t previous) {
	mode(fixture);
	assert(navidrome_sync());
	idle();
	nd_library_t after = library();
	assert(after.ready && after.generation == previous.generation && after.finished == previous.finished);
	assert(after.error && !strcmp(after.error, expected));
}

static void label_checks(nd_library_t lib) {
	char expected[7][ND_TEXT_MAX] = {{0}};
	const int lengths[] = {511, 510, 509, 508, 300, 511, 510};
	for (int i = 0; i < 7; i++) {
		if (i == 4)
			for (int j = 0; j < 100; j++)
				memcpy(expected[i] + j * 3, "界", 3);
		else
			memset(expected[i], 'A' + i - (i > 4), lengths[i]);
	}
	nd_store_t store;
	assert(nd_store_open(&store, lib.path, lib.scope, false, false));
	sqlite3_stmt *st = NULL;
	assert(sqlite3_prepare_v2(store.db, "SELECT title,artist FROM entity WHERE gen=? AND kind=? AND id=?", -1, &st, NULL) == SQLITE_OK);
	for (int kind = ND_ALBUMS; kind <= ND_ARTISTS; kind++) {
		for (int i = 0; i < 7; i++) {
			char id[32];
			snprintf(id, sizeof(id), kind == ND_ALBUMS ? "album-%05d" : kind == ND_TRACKS ? "song-%05d" : "artist-%02d", i);
			sqlite3_reset(st);
			sqlite3_bind_int64(st, 1, lib.generation);
			sqlite3_bind_int(st, 2, kind);
			sqlite3_bind_text(st, 3, id, -1, SQLITE_TRANSIENT);
			assert(sqlite3_step(st) == SQLITE_ROW);
			assert(!strcmp((const char *)sqlite3_column_text(st, 0), expected[i]));
			if (kind != ND_ARTISTS)
				assert(!strcmp((const char *)sqlite3_column_text(st, 1), expected[i]));
		}
	}
	sqlite3_finalize(st);
	nd_store_close(&store);
}

int main(int argc, char **argv) {
	assert(argc == 3 || argc == 4);
	alarm(120);
	snprintf(endpoint, sizeof(endpoint), "%s", argv[1]);
	snprintf(configuration, sizeof(configuration), "%s/settings.ini", argv[2]);
	char host[600], card[1100];
	snprintf(host, sizeof(host), "%s/prefix", endpoint);
	snprintf(card, sizeof(card), "%s/card", argv[2]);
	if (argc == 4) {
		navidrome_init();
		navidrome_set_root(card);
		navidrome_snapshot_t restarted;
		navidrome_snapshot(&restarted);
		assert(restarted.state == ND_LOCKED && !library().ready);
		assert(navidrome_unlock("vault1"));
		restarted = idle();
		assert(restarted.unlocked && restarted.state == ND_DISCONNECTED && library().ready && library().counts[ND_TRACKS] == 17000);
		navidrome_shutdown();
		puts("process restart starts locked and restores snapshot after offline unlock: passed");
		return 0;
	}
	assert(!mkdir(card, 0700));
	navidrome_init();
	navidrome_set_root(card);
	assert(navidrome_save(host, "fixture user", "synthetic-server-password", "vault1"));
	navidrome_snapshot_t s = idle();
	assert(s.state == ND_CONNECTED);
	nd_library_t before = library();
	assert(before.offer && !before.ready && before.writable);
	unsigned offered = before.offer;
	assert(library().offer == offered);
	navidrome_sync_offer_consume(offered);
	assert(!library().offer);
	nd_library_t full = sync_ok(17000, 1700);
	assert(!library().offer);
	query_checks(full);
	artwork_checks(full);
	disk_full_check(full);
	mode("reject");
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		execl(argv[0], argv[0], argv[1], argv[2], "restart", (char *)NULL);
		_exit(127);
	}
	int child_status;
	assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
	mode("normal");
	puts("full library, local windows/order/relationships: passed");

	mode("longartist");
	sync_ok(700, 70);
	mode("longlabels");
	label_checks(sync_ok(700, 70));
	mode("longlabels_raw");
	label_checks(sync_ok(700, 70));
	puts("long display labels, escaped and literal UTF-8, boundary-safe shortening, exact identities: passed");

	mode("structured");
	nd_library_t structured = sync_ok(700, 70);
	nd_store_t structured_store;
	assert(nd_store_open(&structured_store, structured.path, structured.scope, false, false));
	assert(!nd_store_contains(&structured_store, structured.generation, ND_ARTISTS, "legacy-combined-id"));
	assert(!nd_store_contains(&structured_store, structured.generation, ND_GENRES, "Jazz / Rock"));
	nd_row_t structured_rows[ND_WINDOW];
	int structured_count, structured_total;
	nd_query_t structured_query = {.kind = ND_ALBUMS, .filter_kind = ND_ARTISTS, .filter = "artist-00"};
	assert(nd_store_query(&structured_store, structured.generation, &structured_query, 0, structured_rows, &structured_count, &structured_total));
	assert(structured_total == 4);
	nd_store_close(&structured_store);
	mode("nullable");
	sync_ok(700, 70);
	mode("normal");
	full = sync_ok(17000, 1700);
	puts("structured memberships override legacy fields; null optional arrays use legacy fallback: passed");

	rejected("repeat", "nd_sync_inconsistent", full);
	rejected("badref", "nd_sync_inconsistent", full);
	rejected("badartist", "nd_sync_inconsistent", full);
	rejected("badfield", "nd_sync_inconsistent", full);
	rejected("longid", "nd_sync_inconsistent", full);
	rejected("scanning", "nd_sync_inconsistent", full);
	rejected("scanchange", "nd_sync_inconsistent", full);
	rejected("truncated", "nd_response_error", full);
	rejected("oversingle", "nd_page_large", full);
	mode("slow");
	assert(navidrome_sync());
	for (int i = 0; i < 10000 && library().fetched < 50; i++)
		usleep(1000);
	assert(library().fetched >= 50);
	navidrome_sync_cancel();
	idle();
	assert(library().generation == full.generation && !strcmp(library().error, "nd_sync_cancelled"));
	puts("invalid pages, scan changes, size limits, cancellation preserve snapshot: passed");

	mode("reject");
	navidrome_lock();
	assert(!library().ready);
	assert(navidrome_unlock("vault1"));
	s = idle();
	assert(s.unlocked && s.state == ND_DISCONNECTED && library().ready && !library().offer);
	assert(library().generation == full.generation);
	nd_library_t offline = library();
	navidromeart_request(0, &offline, "blue", 32);
	cover_image_t cached = art_wait(0, true);
	cover_free(&cached);
	navidromeart_request(0, &offline, "uncached-offline", 32);
	cached = art_wait(0, false);
	cover_free(&cached);
	assert(!navidrome_sync());
	mode("normal");
	assert(navidrome_reconnect());
	idle();
	assert(library().offer && library().offer != offered);
	navidrome_sync_offer_consume(library().offer);
	assert(navidrome_update_password(host, "fixture user", "synthetic-server-password"));
	idle();
	assert(!library().offer && library().generation == full.generation);
	assert(navidrome_update_password(host, "fixture user", "replacement-fixture-password"));
	s = idle();
	assert(s.state == ND_DISCONNECTED && library().ready && library().generation == full.generation && !strcmp(library().scope, full.scope));
	assert(navidrome_update_password(host, "fixture user", "synthetic-server-password"));
	s = idle();
	assert(s.state == ND_CONNECTED && library().ready && !library().offer);
	puts("offline unlock and operation-specific offers: passed");

	// Read-only cache reads work independently of write eligibility.
	assert(!chmod(card, 0500));
	navidrome_set_root(card);
	for (int i = 0; i < 10000 && library().loading; i++)
		usleep(1000);
	assert(library().ready && !library().writable && !navidrome_sync());
	assert(!chmod(card, 0700));
	navidrome_set_root(card);
	for (int i = 0; i < 10000 && library().loading; i++)
		usleep(1000);
	mode("large");
	sync_ok(700, 70); // Oversized pages reduce their count at the same offset.
	mode("deleted");
	nd_library_t smaller = sync_ok(16990, 1699);
	assert(smaller.generation != full.generation);
	mode("empty");
	nd_library_t empty = sync_ok(0, 0);
	assert(empty.ready && !empty.counts[ND_ARTISTS] && !empty.counts[ND_GENRES]);
	puts("read-only browsing, adaptive page size, deletion and empty refresh: passed");

	mode("normal");
	full = sync_ok(17000, 1700);
	mode("slow");
	assert(navidrome_sync());
	for (int i = 0; i < 10000 && library().fetched < 50; i++)
		usleep(1000);
	navidrome_set_root(NULL);
	idle();
	assert(!library().ready);
	navidrome_set_root(card);
	for (int i = 0; i < 10000 && library().loading; i++)
		usleep(1000);
	assert(library().ready && library().generation == full.generation);
	mode("normal");
	navidrome_set_root(NULL);
	assert(navidrome_forget());
	idle();
	assert(navidrome_save(host, "fixture user", "synthetic-server-password", "vault1"));
	idle();
	navidrome_set_root(card);
	for (int i = 0; i < 10000 && library().loading; i++)
		usleep(1000);
	assert(!library().ready && strcmp(library().scope, full.scope));
	puts("card invalidation and absent-card scope retirement: passed");
	assert(navidrome_forget());
	idle();
	char binding[1200];
	snprintf(binding, sizeof(binding), "%s/navidrome_credentials.json.cache", argv[2]);
	assert(!mkdir(binding, 0700));
	assert(navidrome_save(host, "fixture user", "synthetic-server-password", "vault1"));
	s = idle();
	assert(s.state == ND_CONNECTED && !s.error);
	assert(!library().ready && library().error && !strcmp(library().error, "nd_cache_error") && !navidrome_sync());
	assert(!rmdir(binding));
	assert(navidrome_reconnect());
	idle();
	assert(library().writable && !library().error);
	puts("cache binding failure preserves committed credentials and connectivity: passed");

	full = sync_ok(17000, 1700);
	mode("slow");
	assert(navidrome_sync());
	for (int i = 0; i < 10000 && library().fetched < 50; i++)
		usleep(1000);
	assert(navidrome_forget());
	s = idle();
	assert(!s.configured && !library().ready && access(full.path, F_OK));
	puts("Forget during network work prevents late snapshot publication or recreation: passed");
	struct rusage usage;
	assert(!getrusage(RUSAGE_SELF, &usage));
	printf("host peak RSS: %ld KiB; SQLite peak allocations: %lld bytes; fixed window: %d rows\n", usage.ru_maxrss, (long long)sqlite3_memory_highwater(0), ND_WINDOW);
	navidrome_reset();
	return 0;
}
