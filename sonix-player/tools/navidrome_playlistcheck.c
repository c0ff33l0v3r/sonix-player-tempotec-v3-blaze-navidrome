#include "src/system/net/http.h"
#include "src/system/streaming/navidromeplaylists.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
const char *tr(const char *key) { return key; }
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
static struct {
	void *pointer;
	size_t size;
} allocations[16384];
static size_t live_bytes, peak_bytes;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
static size_t locate(void *p) {
	size_t i = ((uintptr_t)p >> 4) & 16383;
	for (size_t n = 0; n < 16384; n++, i = (i + 1) & 16383) {
		if (allocations[i].pointer == p)
			return i;
		if (!allocations[i].pointer)
			break;
	}
	return 16384;
}
static void forget(size_t i) {
	if (i < 16384) {
		live_bytes -= allocations[i].size;
		allocations[i].pointer = (void *)1;
		allocations[i].size = 0;
	}
}
static void remember(void *p, size_t size) {
	if (!p)
		return;
	size_t i = ((uintptr_t)p >> 4) & 16383;
	while (allocations[i].pointer && allocations[i].pointer != (void *)1)
		i = (i + 1) & 16383;
	allocations[i].pointer = p;
	allocations[i].size = size;
	live_bytes += size;
	if (live_bytes > peak_bytes)
		peak_bytes = live_bytes;
}
void *__wrap_malloc(size_t size) {
	void *p = __real_malloc(size);
	remember(p, size);
	return p;
}
void *__wrap_calloc(size_t count, size_t size) {
	void *p = __real_calloc(count, size);
	remember(p, count * size);
	return p;
}
void *__wrap_realloc(void *p, size_t size) {
	size_t i = locate(p);
	void *r = __real_realloc(p, size);
	if (r) {
		if (live_bytes + size > peak_bytes)
			peak_bytes = live_bytes + size;
		forget(i);
		remember(r, size);
	}
	return r;
}
void __wrap_free(void *p) {
	if (p)
		forget(locate(p));
	__real_free(p);
}

static int leases;
static unsigned epoch = 1, network_epoch = 1;
static bool invalidate_commit, invalidate_after_post;
static int mutation_calls;
bool navidrome_context_current(const nd_context_t *c, bool network) { return c->epoch == epoch && c->card_epoch == 1 && (!network || c->network_epoch == network_epoch); }
bool navidrome_cache_begin(unsigned e, unsigned card) {
	if (e != epoch || card != 1)
		return false;
	leases++;
	return true;
}
void navidrome_cache_end(void) {
	assert(leases > 0);
	leases--;
}
bool navidrome_context_commit(const nd_context_t *c, sqlite3 *db) { return !invalidate_commit && navidrome_context_current(c, false) && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK; }
void navidrome_poll_network(void) {}
bool navidrome_context(nd_context_t *c, bool n) {
	(void)c;
	(void)n;
	return false;
}
void navidrome_library_snapshot(nd_library_t *l) { memset(l, 0, sizeof(*l)); }
bool navidrome_external_begin(const nd_context_t *c, bool n) {
	(void)c;
	(void)n;
	return true;
}
void navidrome_external_end(void) {}
bool gui_post(void (*cb)(void *), void *p) {
	(void)cb;
	(void)p;
	return false;
}
const char *navidrome_validate(const char *h, const char *u, const char *p, char out[ND_HOST_MAX]) {
	if (!*h || !*u || !*p)
		return "nd_fields_required";
	snprintf(out, ND_HOST_MAX, "%s", h);
	return NULL;
}
bool __real_http_request(const char *, const http_req_t *, char **, size_t *, size_t, int, int *);
bool __wrap_http_request(const char *url, const http_req_t *q, char **body, size_t *size, size_t limit, int timeout, int *status) {
	assert(leases == 0);
	bool result = __real_http_request(url, q, body, size, limit, timeout, status);
	if (q->method && !strcmp(q->method, "POST")) {
		mutation_calls++;
		if (invalidate_after_post) {
			invalidate_after_post = false;
			epoch++;
		}
	}
	return result;
}
static nd_context_t context;
static nd_library_t library;
static nd_pl_result_t result;
static void execute(nd_pl_request_t *q) {
	nd_pl_execute(q, &context, &library, &result);
	assert(leases == 0);
}
static void success(nd_pl_request_t *q) {
	execute(q);
	if (result.error)
		fprintf(stderr, "fixture operation=%d error=%s\n", q->action, result.error);
	assert(!result.error);
}
static void control(const char *method) {
	nd_document_t doc;
	assert(!nd_api_playlist(context.host, context.user, context.password, method, NULL, NULL, &doc));
	nd_api_free(&doc);
}
int main(int argc, char **argv) {
	assert(argc == 2);
	snprintf(context.host, sizeof(context.host), "%s", argv[1]);
	strcpy(context.user, "fixture-user");
	strcpy(context.password, "fixture-secret");
	context.epoch = context.card_epoch = context.network_epoch = 1;
	char dir[] = "/tmp/sonix-playlist-service.XXXXXX";
	assert(mkdtemp(dir));
	snprintf(library.path, sizeof(library.path), "%s/library.db", dir);
	strcpy(library.scope, "0123456789abcdef0123456789abcdef");
	library.writable = library.ready = true;
	library.epoch = library.card_epoch = 1;
	nd_store_t lib;
	assert(nd_store_open(&lib, library.path, library.scope, true, false));
	assert(nd_store_begin(&lib));
	library.generation = lib.generation;
	char sql[2000];
	snprintf(sql, sizeof(sql), "INSERT INTO entity(gen,kind,id,title,album,disc,track) VALUES(%lld,0,'album1','Same','',0,0),(%lld,0,'album2','Same','',0,0),(%lld,1,'A','One','album1',1,2),(%lld,1,'B','Two','album1',1,1),(%lld,1,'C','Three','album1',2,1),(%lld,1,'D','Four','album2',1,1)", (long long)lib.generation, (long long)lib.generation, (long long)lib.generation, (long long)lib.generation, (long long)lib.generation, (long long)lib.generation);
	assert(sqlite3_exec(lib.db, sql, NULL, NULL, NULL) == SQLITE_OK && nd_store_publish(&lib));
	nd_store_close(&lib);
	nd_pl_request_t q = {.action = NDPL_CREATE};
	strcpy(q.name, "Local & 海");
	success(&q);
	int64_t local = result.playlist.id;
	// Select albums in reverse tap order, preserve displayed order and expand discs/tracks.
	q = (nd_pl_request_t){.action = NDPL_SELECT, .session = 123, .ordinal = 1, .kind = ND_ALBUMS, .generation = library.generation};
	strcpy(q.track, "album2");
	success(&q);
	q.ordinal = 0;
	strcpy(q.track, "album1");
	success(&q);
	q = (nd_pl_request_t){.action = NDPL_ADD, .id = local, .revision = 1, .session = 123, .generation = library.generation};
	success(&q);
	assert(result.added == 4 && result.skipped == 0);
	q = (nd_pl_request_t){.action = NDPL_ENTRIES, .id = local};
	success(&q);
	assert(result.count == 4 && !strcmp(result.entries[0].track.id, "B") && !strcmp(result.entries[1].track.id, "A") && !strcmp(result.entries[2].track.id, "C") && !strcmp(result.entries[3].track.id, "D"));
	int64_t moving_occurrence = result.entries[0].occurrence;
	int64_t original_revision = result.playlist.revision;
	int original_calls = mutation_calls;
	q = (nd_pl_request_t){.action = NDPL_MOVE, .id = local, .revision = original_revision,
		.ordinal = 0, .first = 3, .occurrence = moving_occurrence};
	strcpy(q.track, "B");
	invalidate_commit = true;
	execute(&q);
	assert(result.error);
	invalidate_commit = false;
	library.writable = false;
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_cache_readonly"));
	library.writable = true;
	network_epoch++; // Move must remain local even with retired transport eligibility.
	success(&q);
	network_epoch--;
	assert(result.added == 1 && result.playlist.revision == original_revision + 1);
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_pl_stale"));
	q.revision++;
	q.ordinal = 3; q.first = 0;
	success(&q);
	assert(mutation_calls == original_calls);
	q = (nd_pl_request_t){.action = NDPL_ENTRIES, .id = local};
	success(&q);
	assert(result.entries[0].occurrence == moving_occurrence && !strcmp(result.entries[3].track.id, "D"));
	int64_t rev = result.playlist.revision;
	q = (nd_pl_request_t){.action = NDPL_SELECT_ONLY, .session = 123, .ordinal = 0, .kind = ND_ALBUMS, .generation = library.generation};
	strcpy(q.track, "album1");
	success(&q);
	q = (nd_pl_request_t){.action = NDPL_ADD, .id = local, .revision = rev, .session = 123, .generation = library.generation};
	success(&q);
	assert(result.added == 0 && result.skipped == 3 && result.playlist.revision == rev);
	// Local commit invalidation rolls back the whole rename.
	q = (nd_pl_request_t){.action = NDPL_RENAME, .id = local, .revision = rev};
	strcpy(q.name, "Stale");
	invalidate_commit = true;
	execute(&q);
	assert(result.error);
	invalidate_commit = false;
	q = (nd_pl_request_t){.action = NDPL_ENTRIES, .id = local};
	success(&q);
	assert(!strcmp(result.playlist.name, "Local & 海"));
	// Import preserves repeated occurrences and does not refresh an existing link.
	q = (nd_pl_request_t){.action = NDPL_CATALOGUE};
	success(&q);
	assert(result.total >= 3);
	q = (nd_pl_request_t){.action = NDPL_IMPORT};
	strcpy(q.remote, "source");
	success(&q);
	int64_t imported = result.playlist.id;
	assert(result.playlist.count == 3 && !result.playlist.dirty);
	q = (nd_pl_request_t){.action = NDPL_RENAME, .id = imported, .revision = 1};
	strcpy(q.name, "Unsent");
	success(&q);
	rev = result.playlist.revision;
	q = (nd_pl_request_t){.action = NDPL_IMPORT};
	strcpy(q.remote, "source");
	success(&q);
	assert(result.playlist.id == imported && !strcmp(result.playlist.name, "Unsent"));
	// Pull stages and confirms a specific revision, then preserves A,B,A.
	q = (nd_pl_request_t){.action = NDPL_PULL, .id = imported, .revision = rev};
	success(&q);
	assert(result.confirm);
	q.action = NDPL_PULL_ACCEPT;
	q.candidate = result.candidate;
	strcpy(q.checked, result.checked);
	success(&q);
	assert(!result.playlist.dirty && result.playlist.count == 3);
	q = (nd_pl_request_t){.action = NDPL_ENTRIES, .id = imported};
	success(&q);
	assert(!strcmp(result.entries[0].track.id, "A") && !strcmp(result.entries[2].track.id, "A"));
	int64_t occurrence = result.entries[0].occurrence;
	rev = result.playlist.revision;
	q = (nd_pl_request_t){.action = NDPL_REMOVE, .id = imported, .revision = rev, .occurrence = occurrence};
	success(&q);
	assert(result.playlist.count == 2 && result.playlist.dirty);
	rev = result.playlist.revision;
	q = (nd_pl_request_t){.action = NDPL_PUBLISH, .id = imported, .revision = rev};
	success(&q);
	assert(result.confirm && !result.conflict);
	q.confirmed = true;
	strcpy(q.checked, result.checked);
	success(&q);
	assert(!result.playlist.dirty);
	// Another client changed server state: confirmation must disclose the conflict.
	control("fixtureChange");
	q.confirmed = false;
	success(&q);
	assert(result.confirm && result.conflict);
	// Readonly imports can be edited locally but are not writable remotely.
	q = (nd_pl_request_t){.action = NDPL_IMPORT};
	strcpy(q.remote, "readonly");
	success(&q);
	int64_t ro = result.playlist.id;
	q = (nd_pl_request_t){.action = NDPL_RENAME, .id = ro, .revision = 1};
	strcpy(q.name, "My copy");
	success(&q);
	q = (nd_pl_request_t){.action = NDPL_PUBLISH, .id = ro, .revision = result.playlist.revision};
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_pl_readonly"));
	// First upload links the returned exact ID and verifies the same local definition.
	q = (nd_pl_request_t){.action = NDPL_UPLOAD, .id = local};
	success(&q);
	assert(*result.playlist.remote && !result.playlist.dirty && !result.playlist.pending);
	// Replacement followed by failed rename records uncertainty; explicit verification sees mismatch.
	rev = result.playlist.revision;
	q = (nd_pl_request_t){.action = NDPL_RENAME, .id = local, .revision = rev};
	strcpy(q.name, "After rename");
	success(&q);
	rev = result.playlist.revision;
	control("fixtureFailRename");
	q = (nd_pl_request_t){.action = NDPL_PUBLISH, .id = local, .revision = rev};
	success(&q);
	q.confirmed = true;
	strcpy(q.checked, result.checked);
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_pl_verify_needed"));
	q = (nd_pl_request_t){.action = NDPL_VERIFY, .id = local};
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_pl_verify_mismatch"));
	q = (nd_pl_request_t){.action = NDPL_PUBLISH, .id = local, .revision = rev};
	success(&q);
	q.confirmed = true;
	strcpy(q.checked, result.checked);
	success(&q);
	assert(!result.playlist.pending);
	// Lost create response blocks ordinary retry and requires explicit candidate reconciliation.
	q = (nd_pl_request_t){.action = NDPL_CREATE};
	strcpy(q.name, "Lost upload");
	success(&q);
	int64_t lost = result.playlist.id;
	control("fixtureLoseCreate");
	q = (nd_pl_request_t){.action = NDPL_UPLOAD, .id = lost};
	execute(&q);
	assert(result.error);
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_pl_verify_needed"));
	q = (nd_pl_request_t){.action = NDPL_RENAME, .id = lost, .revision = 1};
	strcpy(q.name, "Edited after upload failure");
	success(&q);
	assert(result.playlist.pending && !strcmp(result.playlist.intent_name, "Lost upload"));
	q = (nd_pl_request_t){.action = NDPL_CATALOGUE};
	success(&q);
	char recovered[ND_ID_MAX] = "";
	for (int i = 0; i < result.count; i++)
		if (!strcmp(result.playlists[i].name, "Lost upload"))
			strcpy(recovered, result.playlists[i].remote);
	assert(*recovered);
	q = (nd_pl_request_t){.action = NDPL_RECOVER, .id = lost, .confirmed = true};
	strcpy(q.remote, recovered);
	success(&q);
	assert(!strcmp(result.playlist.remote, recovered) && !result.playlist.pending && result.playlist.dirty && !strcmp(result.playlist.name, "Edited after upload failure"));
	// A realistic response above the old body/token limits and POST beyond old URL capacity.
	q = (nd_pl_request_t){.action = NDPL_IMPORT};
	strcpy(q.remote, "large");
	success(&q);
	assert(result.playlist.count == 1000);
	int64_t large = result.playlist.id;
	q = (nd_pl_request_t){.action = NDPL_COPY, .id = large};
	success(&q);
	q = (nd_pl_request_t){.action = NDPL_UPLOAD, .id = result.playlist.id};
	success(&q);
	assert(result.playlist.count == 1000 && !result.playlist.pending);
	// Bounded body/token failures do not affect the local collection.
	nd_document_t doc;
	const char *error = nd_api_playlist(context.host, context.user, context.password, "fixtureOversize", NULL, NULL, &doc);
	assert(error && !strcmp(error, "nd_pl_too_large"));
	error = nd_api_playlist(context.host, context.user, context.password, "fixtureTokens", NULL, NULL, &doc);
	assert(error && !strcmp(error, "nd_pl_too_large"));
	control("fixtureNoPost");
	context.network_epoch = ++network_epoch;
	q = (nd_pl_request_t){.action = NDPL_CREATE};
	strcpy(q.name, "No capability");
	success(&q);
	q = (nd_pl_request_t){.action = NDPL_UPLOAD, .id = result.playlist.id};
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_pl_post_unsupported"));
	// Oversized forms are rejected before journal/mutation, even with thousands of long IDs.
	nd_playlist_store_t store;
	char pl_file[1200];
	assert(nd_pl_path(&library, pl_file) && nd_pl_open(&store, pl_file, library.scope, true));
	int64_t oversize;
	assert(nd_pl_create(&store, "Oversized form", &oversize) && nd_pl_begin(&store, oversize, 1));
	nd_row_t row = {.title = "Long IDs"};
	int added = 0, skipped = 0;
	memset(row.id, '%', 250);
	for (int i = 0; i < 3000; i++) {
		snprintf(row.id + 250, sizeof(row.id) - 250, "%05d", i);
		assert(nd_pl_append(&store, oversize, &row, false, &added, &skipped));
	}
	assert(nd_pl_finish(&store, oversize, true) && nd_pl_sql(&store, "COMMIT"));
	nd_pl_close(&store);
	control("fixtureYesPost");
	context.network_epoch = ++network_epoch;
	int before = mutation_calls;
	q = (nd_pl_request_t){.action = NDPL_UPLOAD, .id = oversize};
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_pl_too_large") && mutation_calls == before);
	// A failed durable journal sends no mutation and does not invent recovery state.
	q = (nd_pl_request_t){.action = NDPL_CREATE};
	strcpy(q.name, "Journal failure");
	success(&q);
	int64_t failed = result.playlist.id;
	invalidate_commit = true;
	q = (nd_pl_request_t){.action = NDPL_UPLOAD, .id = failed};
	execute(&q);
	assert(result.error && mutation_calls == before);
	invalidate_commit = false;
	q.action = NDPL_ENTRIES;
	success(&q);
	assert(!result.playlist.pending);
	// Invalidation after server acceptance leaves a durable unknown outcome on reopen.
	q = (nd_pl_request_t){.action = NDPL_CREATE};
	strcpy(q.name, "Generation changed");
	success(&q);
	int64_t interrupted = result.playlist.id;
	invalidate_after_post = true;
	q = (nd_pl_request_t){.action = NDPL_UPLOAD, .id = interrupted};
	execute(&q);
	assert(result.error && mutation_calls == before + 1);
	context.epoch = epoch;
	library.epoch = epoch;
	q.action = NDPL_ENTRIES;
	success(&q);
	assert(result.playlist.pending && !result.playlist.synced);
	// Missing links remain attached locally; the explicit copy is unlinked.
	control("fixtureDeleteSource");
	q = (nd_pl_request_t){.action = NDPL_PULL, .id = imported};
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_pl_missing"));
	q.action = NDPL_ENTRIES;
	success(&q);
	assert(!strcmp(result.playlist.remote, "source"));
	q.action = NDPL_COPY;
	snprintf(q.name, sizeof(q.name), "Recovered local copy");
	success(&q);
	assert(!*result.playlist.remote && !strcmp(result.playlist.name, "Recovered local copy"));
	// Error bodies may echo secrets, but only fixed API errors leave the boundary.
	error = nd_api_playlist(context.host, context.user, context.password, "fixtureAuth", NULL, NULL, &doc);
	assert(error && !strcmp(error, "nd_auth_error"));
	// Stale scope is rejected without issuing a request.
	epoch++;
	q = (nd_pl_request_t){.action = NDPL_CATALOGUE};
	execute(&q);
	assert(result.error && !strcmp(result.error, "nd_pl_stale"));
	printf("playlist fixture peak tracked allocations (including conservative realloc overlap): %zu bytes\n", peak_bytes);
	assert(peak_bytes <= 8 * 1024 * 1024);
	puts("playlist service: local batch order/dedupe, import/pull/publish, permissions, upload/recovery, 1000 tracks, bounds, stale guards passed");
	char plpath[1200];
	assert(nd_pl_path(&library, plpath));
	unlink(plpath);
	unlink(library.path);
	rmdir(dir);
}
