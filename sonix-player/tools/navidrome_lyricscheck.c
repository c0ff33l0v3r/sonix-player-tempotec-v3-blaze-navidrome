#define _GNU_SOURCE
#include "src/system/streaming/navidromelyrics.h"
#include <assert.h>
#include <dirent.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static _Thread_local long fail_allocation = -1;
static bool fail_commit;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t n);
static bool fail_next(void) { return fail_allocation >= 0 && fail_allocation-- == 0; }
void *__wrap_malloc(size_t n) { return fail_next() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { return fail_next() ? NULL : __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t n) { return fail_next() ? NULL : __real_realloc(p, n); }
int __real_sqlite3_exec(sqlite3 *db, const char *sql, int (*cb)(void *, int, char **, char **), void *user, char **error);
int __wrap_sqlite3_exec(sqlite3 *db, const char *sql, int (*cb)(void *, int, char **, char **), void *user, char **error) {
	if (fail_commit && !strcmp(sql, "COMMIT"))
		return SQLITE_IOERR;
	return __real_sqlite3_exec(db, sql, cb, user, error);
}
static bool current(void *unused);
static atomic_bool dns_started, dns_release, dns_allowed = true;
int __real_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int __wrap_getaddrinfo(const char *host, const char *service, const struct addrinfo *hints, struct addrinfo **result) {
	if (!strcmp(host, "lyrics-stall.invalid")) {
		atomic_store(&dns_started, true);
		while (!atomic_load(&dns_release))
			usleep(1000);
		*result = NULL;
		return EAI_NONAME;
	}
	return __real_getaddrinfo(host, service, hints, result);
}
static bool dns_current(void *unused) {
	(void)unused;
	return atomic_load(&dns_allowed);
}
static void *blocked_dns(void *unused) {
	(void)unused;
	http_session_t *session = http_session_new(dns_current, NULL);
	assert(session && http_session_lyrics(session));
	assert(!http_session_get(session, "http://lyrics-stall.invalid/", false, false));
	http_session_free(session);
	return NULL;
}
static int descriptors(void) {
	DIR *dir = opendir("/proc/self/fd");
	assert(dir);
	int n = 0;
	while (readdir(dir))
		n++;
	closedir(dir);
	return n;
}
static void dns_isolation(const char *host) {
	int before = descriptors();
	pthread_t blocked;
	assert(!pthread_create(&blocked, NULL, blocked_dns, NULL));
	for (int i = 0; i < 1000 && !atomic_load(&dns_started); i++)
		usleep(1000);
	assert(atomic_load(&dns_started));
	atomic_store(&dns_allowed, false);
	pthread_join(blocked, NULL);
	nd_document_t document = {0};
	assert(!nd_api_finite_json(host, "fixture-user", "fixture-secret", "getLyricsBySongId", "id=normal-quota", ND_API_BYTES, current, NULL, &document));
	nd_api_free(&document);
	int status = 0;
	assert(!nd_api_report(host, "fixture-user", "fixture-secret", "getLyricsBySongId", "id=report-quota", current, NULL, &document, &status));
	nd_api_free(&document);
	assert(nd_api_lyrics(host, "fixture-user", "fixture-secret", "lyrics-quota", current, NULL, &document, &status));
	nd_api_free(&document);
	atomic_store(&dns_release, true);
	const char *error = "pending";
	for (int i = 0; i < 100 && error; i++) {
		usleep(1000);
		error = nd_api_lyrics(host, "fixture-user", "fixture-secret", "lyrics-after-dns", current, NULL, &document, &status);
		nd_api_free(&document);
	}
	assert(!error && descriptors() == before);
	puts("lyrics DNS: stalled helper has its own quota; audio/control/report quotas stay available and descriptors drain");
}
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}
const char *tr(const char *s) { return s; }
void nd_log(const char *fmt, ...) { (void)fmt; }
const char *navidrome_validate(const char *host, const char *user, const char *password, char base[ND_HOST_MAX]) {
	(void)user;
	(void)password;
	snprintf(base, ND_HOST_MAX, "%s", host);
	return NULL;
}
static atomic_bool valid = true, idle = true;
static atomic_int leases;
static atomic_uint profile_epoch = 1, card_epoch = 1;
static bool provider_available;
static char provider_ref[80] = "ndq:fixture:0";
static unsigned queue_epoch = 1;
static char api_host[ND_HOST_MAX];
static void (*shutdown_hook)(void);
bool navidrome_playback_current(const nd_context_t *c, bool net) {
	if (net)
		assert(!atomic_load(&leases));
	return atomic_load(&valid) && c->profile_epoch == atomic_load(&profile_epoch) && c->card_epoch == atomic_load(&card_epoch);
}
bool navidrome_playback_begin(const nd_context_t *c) {
	if (!navidrome_playback_current(c, false))
		return false;
	atomic_fetch_add(&leases, 1);
	return true;
}
void navidrome_cache_end(void) { atomic_fetch_sub(&leases, 1); }
bool navidrome_context(nd_context_t *c, bool net) {
	(void)net;
	memset(c, 0, sizeof(*c));
	snprintf(c->host, sizeof(c->host), "%s", api_host);
	strcpy(c->user, "fixture-user");
	strcpy(c->password, "fixture-secret");
	c->profile_epoch = atomic_load(&profile_epoch);
	c->card_epoch = atomic_load(&card_epoch);
	c->playback_network_epoch = 1;
	return atomic_load(&valid);
}
void navidrome_set_lyrics_shutdown(void (*hook)(void)) { shutdown_hook = hook; }
unsigned streamturn_observe(bool *out) {
	*out = atomic_load(&idle);
	return 1;
}
bool navidromeplay_preparing(void) { return false; }
bool navidromeplay_track(const char *p, nd_playrow_t *t, nd_library_t *l) {
	(void)p;
	if (!provider_available)
		return false;
	memset(t, 0, sizeof(*t));
	memset(l, 0, sizeof(*l));
	strcpy(t->row.id, "exact-opaque-id");
	strcpy(t->row.title, "Title");
	strcpy(t->row.artist, "Artist");
	strcpy(l->scope, "0123456789abcdef0123456789abcdef");
	l->card_epoch = atomic_load(&card_epoch);
	return true;
}
bool navidromeplay_lookup(const char *p, nd_playrow_t *t, nd_library_t *l) {
	(void)p;
	(void)t;
	(void)l;
	return false;
}
bool navidromeplay_reference(const char *p) { return p && !strncmp(p, "ndq:", 4); }
bool nd_audio_source(const char *p) { return provider_available && p && p[0] == '/'; }
bool playlist_current_path(char *p, size_t n) {
	snprintf(p, n, "%s", provider_ref);
	return true;
}
unsigned playlist_identity(void) { return queue_epoch; }
static atomic_int rejected_posts;
bool gui_post(void (*done)(void *), void *user) {
	(void)done;
	(void)user;
	atomic_fetch_add(&rejected_posts, 1);
	return false;
}
static bool current(void *unused) {
	(void)unused;
	return atomic_load(&valid);
}
static const char *fixture = "{\"subsonic-response\":{\"status\":\"ok\",\"lyricsList\":{\"structuredLyrics\":[{\"lang\":\"eng\",\"synced\":false,\"line\":[{\"value\":\"plain\"}]},{\"lang\":\"jpn\",\"synced\":true,\"offset\":100,\"line\":[{\"start\":1000,\"value\":\"日本語\"},{\"start\":2000,\"value\":\"\"},{\"start\":3500,\"value\":\"final\"}]}]}}}";
static const char *decode(const char *body, nd_lyric_set_t *set) {
	nd_document_t doc = {0};
	assert(json_parse_limited(body, &doc.doc, 65536));
	doc.response = json_get(&doc.doc, json_root(&doc.doc), "subsonic-response");
	const char *e = nd_lyrics_decode(&doc, set);
	json_free(&doc.doc);
	return e;
}
static void targets(void) {
	nd_lyric_target_t a, b;
	provider_available = true;
	assert(nd_lyrics_target("ndq:fixture:0", &a));
	assert(!a.library.ready && !strcmp(a.id, "exact-opaque-id"));
	assert(!a.context.host[0] && !a.context.user[0] && !a.context.password[0]);
	assert(nd_lyrics_target("/cache/original.flac", &b) && nd_lyrics_same(&a, &b));
	strcpy(provider_ref, "ndq:fixture:1");
	assert(nd_lyrics_target("/permanent/Title.flac", &b) && !nd_lyrics_same(&a, &b));
	strcpy(provider_ref, "ndq:fixture:0");
	queue_epoch++;
	assert(nd_lyrics_target("ndq:fixture:0", &b) && !nd_lyrics_same(&a, &b));
	provider_available = false;
	assert(!nd_lyrics_target("/permanent/Title.flac", &b));
	queue_epoch = 1;
	puts("lyrics targets: no-Sync captured metadata, exact IDs, repeated occurrences, source replacement and same-file Music isolation passed");
}
static void parser(void) {
	nd_lyric_set_t s = {0};
	assert(!decode(fixture, &s));
	assert(s.count == 2 && s.versions[0].lyrics.synced);
	assert(s.versions[0].lyrics.lines[0].ms == 900 && !strcmp(s.versions[0].lyrics.lines[0].text, "日本語"));
	assert(lyrics_line_at(&s.versions[0].lyrics, 1899) == 0 && lyrics_line_at(&s.versions[0].lyrics, 1900) == 1);
	assert(!strcmp(s.versions[0].lyrics.lines[1].text, ""));
	char *text = nd_lyrics_text(&s.versions[0].lyrics);
	lyrics_t l = {0};
	assert(lyrics_parse_text(text, &l));
	assert(l.lines[2].ms == 3400);
	free(text);
	lyrics_free(&l);
	nd_lyrics_free(&s);
	const char *bad[] = {"{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":[{\"synced\":true,\"offset\":-2147483648,\"line\":[{\"start\":1,\"value\":\"x\"}]}]}}}", "{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":[{\"synced\":true,\"line\":[{\"start\":1.5,\"value\":\"x\"}]}]}}}", "{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":[{\"synced\":false,\"line\":[{\"value\":\"x\\u0000y\"}]}]}}}", "{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":[{\"synced\":true,\"synced\":false,\"line\":[]}]}}}"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		const char *e = decode(bad[i], &s);
		if (!e)
			fprintf(stderr, "bad fixture index=%zu accepted\n", i);
		assert(e);
		assert(!s.count);
	}
	char *longtext = malloc(ND_LYRIC_TEXT + 2);
	memset(longtext, 'x', ND_LYRIC_TEXT + 1);
	longtext[ND_LYRIC_TEXT + 1] = 0;
	assert(!lyrics_parse_text(longtext, &l));
	free(longtext);
	char *many = malloc(4002 * 16);
	size_t used = 0;
	for (int i = 0; i < 4001; i++)
		used += (size_t)sprintf(many + used, "[00:01.000]x\n");
	assert(!lyrics_parse_text(many, &l));
	free(many);
	assert(!decode("{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":[{\"synced\":true,\"offset\":-100,\"line\":[{\"start\":1000,\"value\":\"x\"}]}]}}}", &s));
	assert(s.versions[0].lyrics.lines[0].ms == 1100);
	nd_lyrics_free(&s);
	assert(!strcmp(decode("{\"subsonic-response\":{\"lyricsList\":{}}}", &s), "lyrics_not_found"));
	assert(!s.count);
	assert(!strcmp(decode("{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":[]}}}", &s), "lyrics_not_found"));
	assert(!s.count);
	assert(!strcmp(decode("{\"subsonic-response\":{}}", &s), "nd_response_error"));
	assert(!s.count);
	assert(!strcmp(decode("{\"subsonic-response\":{\"lyricsList\":null}}", &s), "nd_response_error"));
	assert(!s.count);
	assert(!strcmp(decode("{\"subsonic-response\":{\"lyricsList\":[]}}", &s), "nd_response_error"));
	assert(!s.count);
	assert(!strcmp(decode("{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":null}}}", &s), "nd_response_error"));
	assert(!s.count);
	assert(!strcmp(decode("{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":{}}}}", &s), "nd_response_error"));
	assert(!s.count);
	assert(!strcmp(decode("{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":[],\"structuredLyrics\":[]}}}", &s), "nd_response_error"));
	puts("lyrics parser: timing, alternatives, Unicode, malformed/duplicate/NUL fields and limits passed");
}
static void boundaries(void) {
	nd_lyric_set_t set = {0};
	for (int fail = 0; fail < 48; fail++) {
		fail_allocation = fail;
		nd_document_t doc = {0};
		bool parsed = json_parse_limited(fixture, &doc.doc, 65536);
		const char *e = "nd_json_memory";
		if (parsed) {
			doc.response = json_get(&doc.doc, json_root(&doc.doc), "subsonic-response");
			e = nd_lyrics_decode(&doc, &set);
			json_free(&doc.doc);
		}
		fail_allocation = -1;
		assert(e ? set.count == 0 : set.count == 2);
		nd_lyrics_free(&set);
	}
	const char *prefix = "{\"subsonic-response\":{\"lyricsList\":{\"structuredLyrics\":[";
	const char *item = "{\"synced\":false,\"line\":[{\"value\":\"x\"}]}";
	char *body = malloc(1024 * 1024 + 1024);
	assert(body);
	for (int count = 8; count <= 9; count++) {
		size_t used = (size_t)sprintf(body, "%s", prefix);
		for (int i = 0; i < count; i++)
			used += (size_t)sprintf(body + used, "%s%s", i ? "," : "", item);
		sprintf(body + used, "]}}}");
		const char *e = decode(body, &set);
		assert(count == 8 ? !e : e != NULL);
		nd_lyrics_free(&set);
	}
	for (int count = 4000; count <= 4001; count++) {
		size_t used = (size_t)sprintf(body, "%s{\"synced\":false,\"line\":[", prefix);
		for (int i = 0; i < count; i++)
			used += (size_t)sprintf(body + used, "%s{\"value\":\"x\"}", i ? "," : "");
		sprintf(body + used, "]}]}}}");
		const char *e = decode(body, &set);
		assert(count == 4000 ? !e : e != NULL);
		nd_lyrics_free(&set);
	}
	for (int count = ND_LYRIC_TEXT - 1; count <= ND_LYRIC_TEXT; count++) {
		size_t used = (size_t)sprintf(body, "%s{\"synced\":false,\"line\":[{\"value\":\"", prefix);
		memset(body + used, 'x', count);
		used += count;
		sprintf(body + used, "\"}]}]}}}");
		const char *e = decode(body, &set);
		assert(count == ND_LYRIC_TEXT - 1 ? !e : e != NULL);
		nd_lyrics_free(&set);
	}
	free(body);
	puts("lyrics boundaries: allocation-failure cleanup and 8/9-version, 4000/4001-line, exact text limits passed");
}
static bool embedded = true;
static unsigned metadata_reads;
char *metadata_read_lyrics(const char *path) {
	(void)path;
	metadata_reads++;
	return embedded ? strdup("embedded words") : NULL;
}
static void local_music(const char *dir) {
	char audio[1200], lrc[1200], text[1200];
	snprintf(audio, sizeof(audio), "%s/music.flac", dir);
	snprintf(lrc, sizeof(lrc), "%s/music.lrc", dir);
	snprintf(text, sizeof(text), "%s/music.txt", dir);
	assert(lrclib_save(audio, "[00:01.125]local chosen"));
	lyrics_t words = {0};
	assert(lyrics_load(audio, "Title", &words));
	assert(words.synced && words.lines[0].ms == 1125 && !metadata_reads);
	lyrics_free(&words);
	assert(!unlink(lrc));
	assert(lyrics_load(audio, "Title", &words));
	assert(!strcmp(words.lines[0].text, "embedded words"));
	lyrics_free(&words);
	embedded = false;
	FILE *f = fopen(text, "wb");
	assert(f);
	const unsigned char latin[] = {'c', 'a', 'f', 0xe9};
	assert(fwrite(latin, 1, sizeof(latin), f) == sizeof(latin));
	assert(!fclose(f));
	assert(lyrics_load(audio, "Title", &words));
	assert(!strcmp(words.lines[0].text, "café"));
	lyrics_free(&words);
	f = fopen(text, "wb");
	assert(f);
	const unsigned char utf16[] = {0xff, 0xfe, 'H', 0, 'i', 0};
	assert(fwrite(utf16, 1, sizeof(utf16), f) == sizeof(utf16));
	assert(!fclose(f));
	assert(lyrics_load(audio, "Title", &words));
	assert(!strcmp(words.lines[0].text, "Hi"));
	lyrics_free(&words);
	puts("Music lyrics: real sidecar save/load priority, embedded fallback and Latin-1/UTF-16 text remain compatible");
}
static void stores(const char *dir) {
	char path[1200];
	snprintf(path, sizeof(path), "%s/lyrics.db", dir);
	const char *scope = "0123456789abcdef0123456789abcdef";
	nd_lyric_set_t s = {0};
	assert(!decode(fixture, &s));
	assert(!nd_lyrics_store(path, scope, "a", ND_LYRIC_VIEW, &s, NULL, 0, current, NULL));
	assert(s.count == 2);
	nd_lyrics_free(&s);
	assert(!nd_lyrics_store(path, scope, "a", ND_LYRIC_APPLY, &s, "[00:00.125]chosen", 0, current, NULL));
	assert(s.override && s.versions[0].lyrics.lines[0].ms == 125);
	nd_lyrics_free(&s);
	assert(!nd_lyrics_store(path, scope, "a", ND_LYRIC_LOCAL, &s, NULL, 0, current, NULL));
	assert(s.override);
	nd_lyrics_free(&s);
	assert(!nd_lyrics_store(path, scope, "a", ND_LYRIC_SERVER, &s, NULL, 0, current, NULL));
	assert(!s.override && s.count == 2);
	nd_lyrics_free(&s);
	assert(!nd_lyrics_store(path, scope, "a", ND_LYRIC_SELECT, &s, NULL, 1, current, NULL));
	assert(s.selected == 1);
	nd_lyrics_free(&s);
	chmod(path, 0400);
	assert(!nd_lyrics_store(path, scope, "a", ND_LYRIC_LOCAL, &s, NULL, 0, current, NULL));
	assert(s.selected == 1);
	nd_lyrics_free(&s);
	assert(nd_lyrics_store(path, scope, "a", ND_LYRIC_APPLY, &s, "new", 0, current, NULL));
	chmod(path, 0600);
	assert(!nd_lyrics_store(path, scope, "orphan", ND_LYRIC_APPLY, &s, "preserve", 0, current, NULL));
	nd_lyrics_free(&s);
	assert(nd_lyrics_store(path, scope, "orphan", ND_LYRIC_SERVER, &s, NULL, 0, current, NULL));
	assert(!nd_lyrics_store(path, scope, "orphan", ND_LYRIC_LOCAL, &s, NULL, 0, current, NULL));
	assert(s.override);
	nd_lyrics_free(&s);
	fail_commit = true;
	assert(nd_lyrics_store(path, scope, "orphan", ND_LYRIC_APPLY, &s, "replacement", 0, current, NULL));
	fail_commit = false;
	assert(!nd_lyrics_store(path, scope, "orphan", ND_LYRIC_LOCAL, &s, NULL, 0, current, NULL));
	assert(s.override && !strcmp(s.versions[0].lyrics.lines[0].text, "preserve"));
	nd_lyrics_free(&s);

	atomic_store(&valid, false);
	assert(nd_lyrics_store(path, scope, "a", ND_LYRIC_APPLY, &s, "new", 0, current, NULL));
	atomic_store(&valid, true);
	assert(!nd_lyrics_store(path, scope, "a", ND_LYRIC_LOCAL, &s, NULL, 0, current, NULL));
	assert(!s.override && s.selected == 1);
	nd_lyrics_free(&s);
	char link[1200];
	snprintf(link, sizeof(link), "%s/link.db", dir);
	assert(!symlink(path, link));
	assert(nd_lyrics_store(link, scope, "a", ND_LYRIC_LOCAL, &s, NULL, 0, current, NULL));
	char unsafe[1200];
	snprintf(unsafe, sizeof(unsafe), "%s/../bad.db", dir);
	assert(nd_lyrics_store(unsafe, scope, "a", ND_LYRIC_APPLY, &s, "x", 0, current, NULL));
	assert(nd_lyrics_store(path, "ffffffffffffffffffffffffffffffff", "a", ND_LYRIC_LOCAL, &s, NULL, 0, current, NULL));
	for (int i = 0; i < 1030; i++) {
		char id[32];
		snprintf(id, sizeof(id), "automatic-%d", i);
		assert(lyrics_parse_text("copy", &s.versions[0].lyrics));
		s.count = 1;
		assert(!nd_lyrics_store(path, scope, id, ND_LYRIC_VIEW, &s, NULL, 0, current, NULL));
		nd_lyrics_free(&s);
	}
	assert(!nd_lyrics_store(path, scope, "override", ND_LYRIC_APPLY, &s, "protected", 0, current, NULL));
	nd_lyrics_free(&s);
	sqlite3 *db = NULL;
	assert(sqlite3_open(path, &db) == SQLITE_OK);
	sqlite3_stmt *st = NULL;
	assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM tracks", -1, &st, NULL) == SQLITE_OK);
	assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) <= 1024);
	sqlite3_finalize(st);
	assert(sqlite3_exec(db, "PRAGMA user_version=999", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
	assert(nd_lyrics_store(path, scope, "override", ND_LYRIC_LOCAL, &s, NULL, 0, current, NULL));
	puts("lyrics store: restart, overrides, server restore, selection, read-only, rollback, scope/path guards, eviction and future schema passed");
}
static int deliveries;
static const char *expected;
static void done(nd_lyric_result_t *r, void *unused) {
	(void)unused;
	assert(!atomic_load(&leases));
	if (expected)
		assert(r->error && !strcmp(r->error, expected));
	else
		assert(!r->error && r->set.count);
	deliveries++;
}
static void wait_result(void) {
	for (int i = 0; i < 500 && !deliveries; i++) {
		nd_lyrics_poll();
		usleep(10000);
	}
	assert(deliveries == 1);
}
static void search_done(nd_lyric_result_t *r, void *unused) {
	(void)unused;
	assert(!r->error && r->count == 1 && r->records[0].synced);
	assert(!atomic_load(&leases));
	deliveries++;
}
static void requests(const char *host, const char *dir) {
	snprintf(api_host, sizeof(api_host), "%s", host);
	nd_lyric_target_t t = {0};
	t.context.profile_epoch = t.context.card_epoch = t.context.playback_network_epoch = 1;
	t.library.card_epoch = 1;
	t.library.writable = true;
	strcpy(t.library.scope, "0123456789abcdef0123456789abcdef");
	snprintf(t.library.path, sizeof(t.library.path), "%s/.cache/library.db", dir);
	strcpy(t.id, "good");
	strcpy(t.reference, "ndq:0:0");
	assert(nd_lyrics_submit(&t, ND_LYRIC_VIEW, NULL, 0, done, NULL));
	wait_result();
	assert(atomic_load(&rejected_posts));
	const char *ids[] = {"empty", "unsupported", "duplicate", "truncated", "redirect", "oversize", "tokens"};
	for (int i = 0; i < 7; i++) {
		strcpy(t.id, ids[i]);
		nd_document_t doc = {0};
		int status = 0;
		const char *e = nd_api_lyrics(host, "fixture-user", "fixture-secret", ids[i], current, NULL, &doc, &status);
		if (i == 0) {
			nd_lyric_set_t set = {0};
			assert(!e);
			assert(nd_lyrics_decode(&doc, &set));
			nd_lyrics_free(&set);
		} else
			assert(e);
		nd_api_free(&doc);
	}
	lrclib_test_api(host);
	strcpy(t.title, "Fixture title");
	strcpy(t.artist, "Fixture artist");
	deliveries = 0;
	assert(nd_lyrics_submit(&t, ND_LYRIC_SEARCH, NULL, 0, search_done, NULL));
	wait_result();
	expected = "nd_busy";
	deliveries = 0;
	strcpy(t.id, "priority");
	atomic_store(&idle, false);
	assert(nd_lyrics_submit(&t, ND_LYRIC_VIEW, NULL, 0, done, NULL));
	wait_result();
	atomic_store(&idle, true);
	unsigned token = nd_lyrics_submit(&t, ND_LYRIC_LOCAL, NULL, 0, done, NULL);
	assert(token);
	nd_lyrics_cancel(token);
	usleep(100000);
	deliveries = 0;
	nd_lyrics_poll();
	assert(!deliveries);
	// Cancel active HTTP and consume one coalesced replacement without stale delivery.
	expected = NULL;
	deliveries = 0;
	strcpy(t.id, "slow");
	token = nd_lyrics_submit(&t, ND_LYRIC_VIEW, NULL, 0, done, NULL);
	assert(token);
	usleep(150000);
	nd_lyrics_cancel(token);
	strcpy(t.id, "good");
	assert(nd_lyrics_submit(&t, ND_LYRIC_LOCAL, NULL, 0, done, NULL));
	wait_result();
	// Replacement card/profile rejects a completion even if access is restored later.
	deliveries = 0;
	strcpy(t.id, "slow-card");
	assert(nd_lyrics_submit(&t, ND_LYRIC_VIEW, NULL, 0, done, NULL));
	usleep(150000);
	atomic_fetch_add(&card_epoch, 1);
	usleep(150000);
	nd_lyrics_poll();
	assert(!deliveries);
	t.context.card_epoch = t.library.card_epoch = atomic_load(&card_epoch);
	strcpy(t.id, "slow-shutdown");
	assert(nd_lyrics_submit(&t, ND_LYRIC_VIEW, NULL, 0, done, NULL));
	usleep(150000);
	assert(shutdown_hook);
	shutdown_hook();
	assert(!atomic_load(&leases));
	dns_isolation(host);
	puts("lyrics worker/API: real loopback, rejected-post fallback, audio priority, cancellation, framing and protected redirects passed");
}
int main(int argc, char **argv) {
	char temp[] = "/tmp/sonix-lyrics-check.XXXXXX";
	char *dir = mkdtemp(temp);
	assert(dir);
	targets();
	parser();
	boundaries();
	local_music(dir);
	stores(dir);
	if (argc > 1)
		requests(argv[1], dir);
	return 0;
}
