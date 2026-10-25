#include "navidromesearch.h"
#include "navidromedebug.h"
#include "src/system/library/library.h"
#include "src/system/net/http.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
	nd_row_t rows[ND_WINDOW];
	int first, count, high, amount, wire_high;
	bool known, end;
	uint64_t fingerprints[4];
	int offsets[4], recent;
} search_window_t;
typedef struct {
	nd_search_source_t source;
	nd_kind_t kind;
	nd_query_t query;
	nd_library_t library;
	nd_row_t *memory;
	sqlite3 *db;
	char path[1200];
	dev_t device;
	ino_t inode;
	int count;
	nd_playrow_t album;
	const char *reason;
} child_t;
struct nd_search {
	char query[ND_SEARCH_QUERY];
	nd_context_t identity;
	nd_search_source_t source;
	nd_library_t library;
	search_window_t windows[3];
	child_t children[2];
	const char *error, *reason;
	bool reset;
	bool (*current)(void *);
	void *user;
};

static bool current(nd_search_t *s) { return (!s->current || s->current(s->user)) && navidrome_search_current(&s->identity, false); }
static bool request_current(void *arg) { return current(arg); }
void nd_search_cancel_check(nd_search_t *s, bool (*cb)(void *), void *user) {
	s->current = cb;
	s->user = user;
}
bool nd_search_valid(nd_search_t *s) { return s && current(s); }

static void child_close(child_t *c) {
	if (c->db)
		sqlite3_close(c->db);
	if (*c->path && navidrome_cache_begin(c->library.epoch, c->library.card_epoch)) {
		struct stat st;
		if (!lstat(c->path, &st) && st.st_dev == c->device && st.st_ino == c->inode && S_ISREG(st.st_mode))
			unlink(c->path);
		navidrome_cache_end();
	}
	free(c->memory);
	memset(c, 0, sizeof(*c));
}
void nd_search_destroy(nd_search_t *s) {
	if (!s)
		return;
	for (int i = 0; i < 2; i++)
		child_close(&s->children[i]);
	nd_wipe(&s->identity, sizeof(s->identity));
	free(s);
}

static void cleanup_children(const nd_library_t *lib) {
	if (!lib->writable || !*lib->path || !navidrome_cache_begin(lib->epoch, lib->card_epoch))
		return;
	char path[1200];
	snprintf(path, sizeof(path), "%s", lib->path);
	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		DIR *dir = opendir(path);
		struct dirent *entry;
		while (dir && (entry = readdir(dir))) {
			if (strlen(entry->d_name) != 13 || strncmp(entry->d_name, "search-", 7))
				continue;
			char file[1500];
			struct stat st;
			snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
			if (!lstat(file, &st) && S_ISREG(st.st_mode))
				unlink(file);
		}
		if (dir)
			closedir(dir);
	}
	navidrome_cache_end();
}

nd_search_t *nd_search_create(const char *query) {
	if (!query || strlen(query) >= ND_SEARCH_QUERY)
		return NULL;
	nd_search_t *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	snprintf(s->query, sizeof(s->query), "%s", query);
	if (!navidrome_context(&s->identity, false)) {
		free(s);
		return NULL;
	}
	nd_wipe(s->identity.password, sizeof(s->identity.password));
	navidrome_snapshot_t state;
	navidrome_snapshot(&state);
	navidrome_library_snapshot(&s->library);
	cleanup_children(&s->library);
	s->source = state.state == ND_CONNECTED ? ND_SEARCH_SERVER : ND_SEARCH_SYNCED;
	if (s->source == ND_SEARCH_SYNCED && !s->library.ready)
		s->error = s->library.error ? s->library.error : "nd_not_synced";
	bool nonblank = false;
	for (const unsigned char *p = (const unsigned char *)query; *p; p++)
		nonblank |= !isspace(*p);
	if (!nonblank)
		s->source = ND_SEARCH_NONE;
	for (int i = 0; i < 3; i++)
		s->windows[i].amount = ND_WINDOW;
	return s;
}

// Reuse the store's exact ID and UTF-8 display-field policy for remote rows.
static bool string(const json_doc_t *d, int obj, const char *key, char *out, size_t cap, bool required, bool display) {
	int t = json_get(d, obj, key);
	out[0] = 0;
	if (t < 0 || d->toks[t].type == JSON_NULL)
		return !required;
	if (d->toks[t].type != JSON_STRING)
		return false;
	if (!display && (size_t)(d->toks[t].end - d->toks[t].start) >= cap)
		return false;
	for (int i = d->toks[t].start; i + 6 <= d->toks[t].end; i++)
		if (!memcmp(d->text + i, "\\u0000", 6))
			return false;
	char decoded[ND_TEXT_MAX + 4];
	if (!json_str(d, t, display ? decoded : out, display ? sizeof(decoded) : cap))
		return false;
	if (display) {
		size_t n = strlen(decoded);
		if (n >= cap) {
			n = cap - 1;
			while (n && ((unsigned char)decoded[n] & 0xc0) == 0x80)
				n--;
		}
		memcpy(out, decoded, n);
		out[n] = 0;
	}
	return !required || *out;
}
static bool row_read(const json_doc_t *d, int obj, nd_kind_t kind, nd_row_t *row) {
	memset(row, 0, sizeof(*row));
	if (obj < 0 || d->toks[obj].type != JSON_OBJECT || !string(d, obj, "id", row->id, sizeof(row->id), true, false) || !string(d, obj, kind == ND_TRACKS ? "title" : "name", row->title, sizeof(row->title), false, true) || !string(d, obj, "artist", row->artist, sizeof(row->artist), false, true) || !string(d, obj, "coverArt", row->cover, sizeof(row->cover), false, false))
		return false;
	if (!*row->title)
		snprintf(row->title, sizeof(row->title), "%s", row->id);
	const char *keys[] = {"duration", "discNumber", "track"};
	int *values[] = {&row->duration, &row->disc, &row->track};
	for (int i = 0; i < 3; i++) {
		int t = json_get(d, obj, keys[i]);
		if (t >= 0 && d->toks[t].type != JSON_NUMBER && d->toks[t].type != JSON_NULL)
			return false;
		long long n = json_llong(d, t, 0);
		if (n < 0 || n > INT_MAX)
			return false;
		*values[i] = (int)n;
	}
	return true;
}
static int array(const json_doc_t *d, int obj, const char *key) {
	if (obj < 0 || d->toks[obj].type != JSON_OBJECT)
		return -2;
	int a = json_get(d, obj, key);
	return a < 0 ? -1 : d->toks[a].type == JSON_ARRAY ? a : -2;
}
static bool context(nd_search_t *s, nd_context_t *ctx) {
	if (!current(s) || !navidrome_context(ctx, true))
		return false;
	if (ctx->profile_epoch != s->identity.profile_epoch) {
		nd_wipe(ctx, sizeof(*ctx));
		return false;
	}
	return true;
}

static const char *artist_albums(nd_search_t *s, const json_doc_t *doc, int obj, const char *id, bool *available) {
	int token = json_get(doc, obj, "albumCount");
	if (token >= 0 && doc->toks[token].type != JSON_NULL) {
		long long n = json_llong(doc, token, -1);
		if (doc->toks[token].type != JSON_NUMBER || n < 0 || n > INT_MAX)
			return "nd_response_error";
		*available = n > 0;
		return NULL;
	}
	nd_context_t ctx;
	if (!context(s, &ctx))
		return "nd_sync_cancelled";
	char encoded[ND_ID_MAX * 3 + 1], query[ND_ID_MAX * 3 + 8];
	http_url_encode(id, encoded, sizeof(encoded));
	snprintf(query, sizeof(query), "id=%s", encoded);
	nd_document_t child;
	const char *error = nd_api_finite_json(ctx.host, ctx.user, ctx.password, "getArtist", query, ND_API_BYTES, request_current, s, &child);
	if (!error) {
		int artist = json_get(&child.doc, child.response, "artist");
		char returned[ND_ID_MAX];
		int albums = array(&child.doc, artist, "album");
		if (!navidrome_search_current(&ctx, true) || !string(&child.doc, artist, "id", returned, sizeof(returned), true, false) || strcmp(id, returned) || albums == -2)
			error = "nd_response_error";
		else
			*available = json_len(&child.doc, albums) > 0;
		nd_api_free(&child);
	}
	nd_wipe(&ctx, sizeof(ctx));
	return error;
}

static const char *server_page(nd_search_t *s, nd_kind_t kind, int first, search_window_t *w, bool eligible[ND_WINDOW]) {
	char encoded[ND_SEARCH_QUERY * 3 + 1], query[ND_SEARCH_QUERY * 3 + 240];
	http_url_encode(s->query, encoded, sizeof(encoded));
	nd_document_t doc;
	const char *error;
	for (;;) {
		nd_context_t ctx;
		if (!context(s, &ctx))
			return "nd_network_error";
		snprintf(query, sizeof(query), "query=%s&songCount=%d&songOffset=%d&albumCount=%d&albumOffset=%d&artistCount=%d&artistOffset=%d", encoded, kind == ND_TRACKS ? w->amount : 0, first, kind == ND_ALBUMS ? w->amount : 0, first, kind == ND_ARTISTS ? w->amount : 0, first);
		error = nd_api_finite_json(ctx.host, ctx.user, ctx.password, "search3", query, ND_API_BYTES, request_current, s, &doc);
		s->identity.search_network_epoch = ctx.search_network_epoch;
		if (!error && !navidrome_search_current(&ctx, true)) {
			nd_api_free(&doc);
			error = "nd_network_error";
		}
		nd_wipe(&ctx, sizeof(ctx));
		if (!current(s)) {
			if (!error)
				nd_api_free(&doc);
			return "nd_sync_cancelled";
		}
		if (!error || strcmp(error, "nd_page_large") || w->amount == 1)
			break;
		w->amount = (w->amount + 1) / 2;
	}
	if (error)
		return error;
	const char *keys[] = {"album", "song", "artist"};
	int obj = json_get(&doc.doc, doc.response, "searchResult3");
	int a = array(&doc.doc, obj, keys[kind]);
	int n = json_len(&doc.doc, a);
	if (a == -2 || n > w->amount)
		error = "nd_response_error";
	uint64_t fingerprint = 1469598103934665603ULL;
	for (int i = 0; !error && i < n; i++) {
		if (!row_read(&doc.doc, json_at(&doc.doc, a, i), kind, &w->rows[i])) {
			error = "nd_response_error";
			break;
		}
		if (eligible && (error = artist_albums(s, &doc.doc, json_at(&doc.doc, a, i), w->rows[i].id, &eligible[i])))
			break;
		for (int j = 0; j < i; j++)
			if (!strcmp(w->rows[j].id, w->rows[i].id))
				error = "nd_sync_inconsistent";
		for (const unsigned char *p = (const unsigned char *)w->rows[i].id; *p; p++)
			fingerprint = (fingerprint ^ *p) * 1099511628211ULL;
		fingerprint = (fingerprint ^ 0xff) * 1099511628211ULL;
	}
	if (!error && n) {
		for (int i = 0; i < 4; i++)
			if (w->fingerprints[i] == fingerprint && w->offsets[i] != first)
				error = "nd_sync_inconsistent";
		int at = w->recent++ % 4;
		w->fingerprints[at] = fingerprint;
		w->offsets[at] = first;
	}
	if (!error) {
		w->first = first;
		w->count = n;
		w->known = true;
		if (first <= w->high && first + n >= w->high) {
			w->high = first + n;
			if (n < w->amount)
				w->end = true;
		}
	}
	nd_log("navidrome: search page kind=%d offset=%d requested=%d accepted=%d query_bytes=%zu result=%s\n", kind, first, w->amount, error ? 0 : n, strlen(s->query), error ? error : "ok");
	nd_api_free(&doc);
	return error;
}

// Collect smaller accepted protocol pages into one fixed visible window.
static const char *artist_window(nd_search_t *s, int first, search_window_t *w) {
	// The server offset includes hidden credits; the visible offset does not.
	// Earlier evicted windows rescan from zero with fixed memory, retaining order.
	search_window_t raw = *w;
	raw.high = w->wire_high;
	raw.end = false;
	int logical = first == w->high ? first : 0;
	int wire = first == w->high ? w->wire_high : 0;
	int filled = 0;
	bool ended = false;
	nd_row_t rows[ND_WINDOW];
	while (filled < ND_WINDOW && !ended) {
		if (wire > INT_MAX - ND_WINDOW)
			return "nd_sync_inconsistent";
		bool eligible[ND_WINDOW] = {0};
		const char *error = server_page(s, ND_ARTISTS, wire, &raw, eligible);
		if (error)
			return error;
		int i = 0;
		for (; i < raw.count && filled < ND_WINDOW; i++) {
			wire++;
			if (!eligible[i])
				continue;
			if (logical >= first)
				rows[filled++] = raw.rows[i];
			logical++;
		}
		ended = i == raw.count && raw.count < raw.amount;
		if (!current(s))
			return "nd_sync_cancelled";
	}
	if (logical >= w->high) {
		w->high = logical;
		w->wire_high = wire;
		w->end = ended;
	}
	w->amount = raw.amount;
	w->recent = raw.recent;
	memcpy(w->fingerprints, raw.fingerprints, sizeof(w->fingerprints));
	memcpy(w->offsets, raw.offsets, sizeof(w->offsets));
	if (filled || !first) {
		w->first = first;
		w->count = filled;
		memcpy(w->rows, rows, (size_t)filled * sizeof(*rows));
	}
	w->known = true;
	return NULL;
}

static const char *server_window(nd_search_t *s, nd_kind_t kind, int first, search_window_t *w) {
	if (kind == ND_ARTISTS)
		return artist_window(s, first, w);
	nd_row_t rows[ND_WINDOW];
	int filled = 0, old_first = w->first, old_count = w->count;
	memcpy(rows, w->rows, sizeof(rows));
	while (filled < ND_WINDOW) {
		// Never request more than the remaining fixed destination capacity.
		int amount = w->amount;
		if (w->amount > ND_WINDOW - filled)
			w->amount = ND_WINDOW - filled;
		const char *error = server_page(s, kind, first + filled, w, NULL);
		if (w->amount == ND_WINDOW - filled)
			w->amount = amount;
		if (error)
			return error;
		memcpy(rows + filled, w->rows, (size_t)w->count * sizeof(nd_row_t));
		filled += w->count;
		if ((w->end && first + filled >= w->high) || !w->count)
			break;
	}
	if (!filled && first > 0 && w->end) {
		w->first = old_first;
		w->count = old_count;
	} else {
		w->first = first;
		w->count = filled;
	}
	memcpy(w->rows, rows, sizeof(rows));
	return NULL;
}

static bool local_window(nd_search_t *s, nd_kind_t kind, int first, search_window_t *w) {
	nd_store_t store = {0};
	nd_query_t q = {.kind = kind};
	snprintf(q.search, sizeof(q.search), "%s", s->query);
	bool lease = navidrome_cache_begin(s->library.epoch, s->library.card_epoch);
	bool ok = lease && nd_store_open(&store, s->library.path, s->library.scope, false, false) && nd_store_query(&store, s->library.generation, &q, first, w->rows, &w->count, &w->high);
	nd_store_close(&store);
	if (lease)
		navidrome_cache_end();
	w->first = first;
	w->end = true;
	w->known = ok;
	return ok;
}
static void local_reset(nd_search_t *s) {
	s->error = NULL;
	memset(s->windows, 0, sizeof(s->windows));
	for (int i = 0; i < 2; i++)
		child_close(&s->children[i]);
	s->source = ND_SEARCH_SYNCED;
	s->reset = true;
	navidrome_library_snapshot(&s->library);
	if (!s->library.ready) {
		s->error = s->library.error ? s->library.error : "nd_not_synced";
		s->source = ND_SEARCH_NONE;
		return;
	}
	for (int k = 0; k < 3 && current(s); k++)
		if (!local_window(s, (nd_kind_t)k, 0, &s->windows[k])) {
			s->error = "nd_cache_error";
			return;
		}
}
static void result(nd_search_t *s, nd_kind_t kind, nd_search_result_t *out) {
	out->kind = kind;
	out->source = s->source;
	out->library = s->library;
	out->error = s->error;
	out->reason = s->reason;
	out->reset = s->reset;
	s->reset = false;
	for (int k = 0; k < 3; k++) {
		out->totals[k] = s->windows[k].known ? s->windows[k].high : -1;
		out->remaining[k] = !s->windows[k].end;
	}
	search_window_t *w = &s->windows[kind];
	out->first = w->first;
	out->count = s->error ? 0 : w->count;
	out->total = w->high;
	out->more = !w->end;
	memcpy(out->rows, w->rows, sizeof(out->rows));
}

void nd_search_read(nd_search_t *s, nd_kind_t kind, int first, nd_search_result_t *out) {
	memset(out, 0, sizeof(*out));
	if (!s || (unsigned)kind > ND_ARTISTS || first < 0 || first > INT_MAX - ND_WINDOW) {
		out->error = "nd_response_error";
		return;
	}
	if (!current(s)) {
		out->error = "nd_sync_cancelled";
		return;
	}
	if (s->source == ND_SEARCH_NONE) {
		result(s, kind, out);
		return;
	}
	if (s->source == ND_SEARCH_SYNCED) {
		nd_library_t now;
		navidrome_library_snapshot(&now);
		bool refresh = s->windows[kind].known && now.epoch != s->library.epoch;
		if (now.epoch != s->library.epoch || !s->windows[kind].known)
			local_reset(s);
		if (refresh)
			s->reset = false;
		if (first >= s->windows[kind].high)
			first = s->windows[kind].high > ND_WINDOW ? s->windows[kind].high - ND_WINDOW : 0;
		if (!s->error && !local_window(s, kind, s->reset ? 0 : first, &s->windows[kind]))
			s->error = "nd_cache_error";
	} else if (!s->error) {
		navidrome_library_snapshot(&s->library);
		search_window_t *w = &s->windows[kind];
		if (w->known && first == w->first) {
			result(s, kind, out);
			return;
		}
		// Sequential frontier bounds fetches and makes lower-bound counts honest.
		if (first > w->high)
			first = w->high;
		const char *error = server_window(s, kind, first, w);
		if (error && current(s)) {
			s->reason = error;
			if (!strcmp(error, "nd_network_error") || !strcmp(error, "nd_tls_error") || !strcmp(error, "nd_auth_error"))
				navidrome_search_failed(&s->identity, error);
			local_reset(s);
		} else if (error)
			s->error = error;
	}
	if (!current(s))
		s->error = "nd_sync_cancelled";
	result(s, kind, out);
}

static int row_order(const void *a, const void *b) {
	const nd_row_t *x = a, *y = b;
	if (x->disc != y->disc)
		return (x->disc > y->disc) - (x->disc < y->disc);
	if (x->track != y->track)
		return (x->track > y->track) - (x->track < y->track);
	char ka[LIBRARY_SORT_KEY_MAX], kb[LIBRARY_SORT_KEY_MAX];
	library_sort_key_ex(x->title, ka, sizeof(ka), true);
	library_sort_key_ex(y->title, kb, sizeof(kb), true);
	int n = strcmp(ka, kb);
	return n ? n : strcmp(x->id, y->id);
}
static bool child_storage(nd_search_t *s, child_t *c, int n) {
	if (n <= ND_SEARCH_CHILD_ROWS) {
		c->memory = calloc(n ? (size_t)n : 1, sizeof(nd_row_t));
		return c->memory != NULL;
	}
	if (!c->library.writable || !*c->library.path)
		return false;
	// The caller holds a short card lease through creation, import, and close.
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s", c->library.path);
	char *slash = strrchr(dir, '/');
	bool ok = slash != NULL && current(s);
	if (ok) {
		*slash = 0;
		for (char *p = dir + 1; *p; p++)
			if (*p == '/') {
				*p = 0;
				mkdir(dir, 0700);
				*p = '/';
			}
		mkdir(dir, 0700);
		snprintf(c->path, sizeof(c->path), "%s/search-XXXXXX", dir);
		int fd = mkstemp(c->path);
		ok = fd >= 0;
		if (fd >= 0) {
			struct stat st;
			if (!fstat(fd, &st)) {
				c->device = st.st_dev;
				c->inode = st.st_ino;
			}
			close(fd);
		}
		if (ok)
			ok = sqlite3_open(c->path, &c->db) == SQLITE_OK && sqlite3_exec(c->db,
																			"PRAGMA page_size=4096; PRAGMA max_page_count=512; PRAGMA cache_size=-64; PRAGMA journal_mode=MEMORY;"
																			"CREATE TABLE row(id TEXT PRIMARY KEY,disc INT,track INT,key TEXT,data BLOB); CREATE INDEX ordered ON row(disc,track,key,id); BEGIN;",
																			NULL, NULL, NULL) == SQLITE_OK;
	}
	return ok;
}
static bool child_insert(child_t *c, const nd_row_t *row, int i) {
	if (c->memory) {
		for (int j = 0; j < i; j++)
			if (!strcmp(c->memory[j].id, row->id))
				return false;
		c->memory[i] = *row;
		return true;
	}
	sqlite3_stmt *st = NULL;
	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key_ex(row->title, key, sizeof(key), true);
	bool ok = sqlite3_prepare_v2(c->db, "INSERT INTO row VALUES(?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_text(st, 1, row->id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 2, row->disc);
		sqlite3_bind_int(st, 3, row->track);
		sqlite3_bind_text(st, 4, key, -1, SQLITE_TRANSIENT);
		sqlite3_bind_blob(st, 5, row, sizeof(*row), SQLITE_TRANSIENT);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
static bool local_child(nd_search_t *s, child_t *c, nd_kind_t parent, const char *id) {
	navidrome_library_snapshot(&c->library);
	if (!current(s) || !c->library.ready)
		return false;
	nd_store_t store = {0};
	bool lease = navidrome_cache_begin(c->library.epoch, c->library.card_epoch);
	bool ok = lease && nd_store_open(&store, c->library.path, c->library.scope, false, false) && nd_store_contains(&store, c->library.generation, parent, id);
	if (ok && parent == ND_ALBUMS)
		ok = nd_store_metadata(&store, c->library.generation, ND_ALBUMS, id, &c->album);
	nd_store_close(&store);
	if (lease)
		navidrome_cache_end();
	if (!ok)
		return false;
	c->source = ND_SEARCH_SYNCED;
	c->query.kind = c->kind;
	c->query.filter_kind = parent;
	snprintf(c->query.filter, sizeof(c->query.filter), "%s", id);
	return true;
}
void nd_search_child(nd_search_t *s, int level, nd_kind_t parent, const char *id, nd_search_result_t *out) {
	memset(out, 0, sizeof(*out));
	if (!s || level < 1 || level > 2 || parent == ND_TRACKS || (unsigned)parent > ND_ARTISTS || !id || !*id || strlen(id) >= ND_ID_MAX) {
		out->error = "nd_response_error";
		return;
	}
	child_t *c = &s->children[level - 1];
	child_close(c);
	c->kind = parent == ND_ALBUMS ? ND_TRACKS : ND_ALBUMS;
	bool server = level == 1 ? s->source == ND_SEARCH_SERVER : s->children[level - 2].source == ND_SEARCH_SERVER;
	const char *error = NULL;
	if (server) {
		nd_context_t ctx;
		nd_document_t doc;
		char encoded[ND_ID_MAX * 3 + 1], query[ND_ID_MAX * 3 + 8];
		http_url_encode(id, encoded, sizeof(encoded));
		snprintf(query, sizeof(query), "id=%s", encoded);
		if (!context(s, &ctx))
			error = "nd_network_error";
		else {
			error = nd_api_finite_json(ctx.host, ctx.user, ctx.password, parent == ND_ARTISTS ? "getArtist" : "getAlbum", query, ND_API_BYTES, request_current, s, &doc);
			if (!error && !navidrome_search_current(&ctx, true)) {
				nd_api_free(&doc);
				error = "nd_network_error";
			}
			nd_wipe(&ctx, sizeof(ctx));
			if (!error) {
				int obj = json_get(&doc.doc, doc.response, parent == ND_ARTISTS ? "artist" : "album");
				char returned[ND_ID_MAX];
				if (!string(&doc.doc, obj, "id", returned, sizeof(returned), true, false) || strcmp(id, returned))
					error = "nd_response_error";
				if (!error && parent == ND_ALBUMS && !nd_metadata_row(&doc.doc, obj, ND_ALBUMS, &c->album))
					error = "nd_response_error";
				int a = array(&doc.doc, obj, parent == ND_ARTISTS ? "album" : "song");
				int n = json_len(&doc.doc, a);
				if (a == -2)
					error = "nd_response_error";
				navidrome_library_snapshot(&c->library);
				bool disk = n > ND_SEARCH_CHILD_ROWS;
				bool lease = !error && disk && navidrome_cache_begin(c->library.epoch, c->library.card_epoch);
				if (!error && disk && !lease)
					error = "nd_cache_storage";
				if (!error && !child_storage(s, c, n))
					error = "nd_search_child_limit";
				for (int i = 0; !error && i < n; i++) {
					nd_row_t row;
					if (!current(s))
						error = "nd_sync_cancelled";
					else if (!row_read(&doc.doc, json_at(&doc.doc, a, i), c->kind, &row))
						error = "nd_response_error";
					else if (!child_insert(c, &row, i))
						error = "nd_search_child_limit";
				}
				if (c->db && !error && sqlite3_exec(c->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
					error = "nd_cache_error";
				if (c->db) {
					sqlite3_close(c->db);
					c->db = NULL;
				}
				if (lease)
					navidrome_cache_end();
				if (!error) {
					c->count = n;
					c->source = ND_SEARCH_SERVER;
					if (c->memory)
						qsort(c->memory, n, sizeof(nd_row_t), row_order);
				}
				nd_api_free(&doc);
			}
		}
	}
	if (!server || error) {
		nd_kind_t kind = c->kind;
		child_close(c);
		c->kind = kind;
		c->reason = error;
		if (!local_child(s, c, parent, id)) {
			out->error = error ? error : "nd_item_removed";
			return;
		}
	}
	nd_search_child_read(s, level, 0, out);
}
void nd_search_child_read(nd_search_t *s, int level, int first, nd_search_result_t *out) {
	memset(out, 0, sizeof(*out));
	if (!s || level < 1 || level > 2 || first < 0 || !current(s)) {
		out->error = "nd_sync_cancelled";
		return;
	}
	child_t *c = &s->children[level - 1];
	out->card_backed = *c->path != 0;
	out->kind = c->kind;
	out->first = first;
	out->source = c->source;
	out->reason = c->reason;
	out->library = c->library;
	out->album.info = c->album;
	if (c->source == ND_SEARCH_SYNCED) {
		nd_store_t store = {0};
		bool lease = navidrome_cache_begin(c->library.epoch, c->library.card_epoch);
		bool ok = lease && nd_store_open(&store, c->library.path, c->library.scope, false, false) && nd_store_query(&store, c->library.generation, &c->query, first, out->rows, &out->count, &out->total);
		nd_store_close(&store);
		if (lease)
			navidrome_cache_end();
		if (!ok)
			out->error = "nd_cache_error";
	} else if (c->memory) {
		out->total = c->count;
		for (int i = first; i < c->count && out->count < ND_WINDOW; i++)
			out->rows[out->count++] = c->memory[i];
	} else if (*c->path) {
		out->total = c->count;
		bool lease = navidrome_cache_begin(c->library.epoch, c->library.card_epoch);
		struct stat stbuf;
		sqlite3_stmt *st = NULL;
		bool ok = lease && !lstat(c->path, &stbuf) && S_ISREG(stbuf.st_mode) && stbuf.st_dev == c->device && stbuf.st_ino == c->inode && sqlite3_open_v2(c->path, &c->db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && sqlite3_exec(c->db, "PRAGMA cache_size=-64", NULL, NULL, NULL) == SQLITE_OK && sqlite3_prepare_v2(c->db, "SELECT data FROM row ORDER BY disc,track,key,id LIMIT ? OFFSET ?", -1, &st, NULL) == SQLITE_OK;
		if (ok) {
			sqlite3_bind_int(st, 1, ND_WINDOW);
			sqlite3_bind_int(st, 2, first);
			int rc;
			while ((rc = sqlite3_step(st)) == SQLITE_ROW && out->count < ND_WINDOW) {
				if (sqlite3_column_bytes(st, 0) != sizeof(nd_row_t)) {
					ok = false;
					break;
				}
				memcpy(&out->rows[out->count++], sqlite3_column_blob(st, 0), sizeof(nd_row_t));
			}
			ok = ok && rc == SQLITE_DONE;
		}
		sqlite3_finalize(st);
		if (c->db) {
			sqlite3_close(c->db);
			c->db = NULL;
		}
		if (lease)
			navidrome_cache_end();
		if (!ok)
			out->error = "nd_cache_storage";
	} else
		out->error = "nd_item_removed";
	if (!current(s)) {
		out->count = 0;
		out->error = "nd_sync_cancelled";
	}
}

static int child_disc(int disc) { return disc > 0 ? disc : 1; }

static const char *child_album_cte = "WITH t AS (SELECT id,CASE WHEN disc>0 THEN disc ELSE 1 END AS disc,data,row_number() OVER(ORDER BY disc,track,key,id)-1 AS ordinal,"
									 "dense_rank() OVER(ORDER BY CASE WHEN disc>0 THEN disc ELSE 1 END)-1 AS dr FROM row),"
									 "d AS (SELECT disc,min(ordinal) AS ordinal,dr FROM t GROUP BY disc),"
									 "v AS (SELECT data,ordinal,disc,0 AS heading,ordinal*?1+CASE WHEN (SELECT count(*) FROM d)>1 THEN (dr+1)*?2 ELSE 0 END AS y FROM t "
									 "UNION ALL SELECT NULL,ordinal,disc,1,ordinal*?1+dr*?2 FROM d WHERE (SELECT count(*) FROM d)>1) ";

void nd_search_child_album_read(nd_search_t *s, int level, int offset, int pitch, int disc_pitch, bool alternate, nd_search_result_t *out) {
	if (!alternate) {
		nd_search_child_read(s, level, offset, out);
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!s || level < 1 || level > 2 || offset < 0 || pitch <= 0 || disc_pitch <= 0 || !current(s)) {
		out->error = "nd_sync_cancelled";
		return;
	}
	child_t *c = &s->children[level - 1];
	if (c->kind != ND_TRACKS) {
		out->error = "nd_response_error";
		return;
	}
	out->alternate = true;
	out->kind = c->kind;
	out->source = c->source;
	out->reason = c->reason;
	out->library = c->library;
	out->card_backed = *c->path != 0;
	out->album.info = c->album;
	out->album.offset = offset;
	bool ok = true;
	if (c->source == ND_SEARCH_SYNCED) {
		nd_store_t store = {0};
		bool lease = navidrome_cache_begin(c->library.epoch, c->library.card_epoch);
		ok = lease && nd_store_open(&store, c->library.path, c->library.scope, false, false) && nd_store_album_window(&store, c->library.generation, &c->query, offset, pitch, disc_pitch, out->rows, &out->count, &out->total, &out->album);
		nd_store_close(&store);
		if (lease)
			navidrome_cache_end();
	} else if (c->source == ND_SEARCH_SERVER && c->memory) {
		int groups = 0;
		for (int i = 0; i < c->count; i++)
			if (!i || child_disc(c->memory[i].disc) != child_disc(c->memory[i - 1].disc))
				groups++;
		int64_t extent = (int64_t)c->count * pitch + (groups > 1 ? (int64_t)groups * disc_pitch : 0);
		ok = extent <= INT_MAX;
		out->total = c->count;
		out->album.extent = ok ? (int)extent : 0;
		int y = 0;
		for (int i = 0; ok && i < c->count; i++) {
			if (groups > 1 && (!i || child_disc(c->memory[i].disc) != child_disc(c->memory[i - 1].disc))) {
				if (y + disc_pitch > offset && out->count < ND_WINDOW) {
					int n = out->count++;
					out->rows[n].disc = child_disc(c->memory[i].disc);
					out->album.heading[n] = true;
					out->album.y[n] = y;
					out->album.ordinal[n] = i;
				}
				y += disc_pitch;
			}
			if (y + pitch > offset && out->count < ND_WINDOW) {
				int n = out->count++;
				out->rows[n] = c->memory[i];
				out->album.y[n] = y;
				out->album.ordinal[n] = i;
			}
			y += pitch;
		}
	} else if (c->source == ND_SEARCH_SERVER && *c->path) {
		bool lease = navidrome_cache_begin(c->library.epoch, c->library.card_epoch);
		struct stat sb;
		ok = lease && !lstat(c->path, &sb) && S_ISREG(sb.st_mode) && sb.st_dev == c->device && sb.st_ino == c->inode && sqlite3_open_v2(c->path, &c->db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && sqlite3_exec(c->db, "PRAGMA cache_size=-64; PRAGMA temp_store=FILE", NULL, NULL, NULL) == SQLITE_OK;
		char text[1600];
		for (int pass = 0; ok && pass < 2; pass++) {
			snprintf(text, sizeof(text), "%s%s", child_album_cte, pass ? "SELECT data,ordinal,disc,heading,y FROM v WHERE y+CASE WHEN heading THEN ?2 ELSE ?1 END>?3 ORDER BY y LIMIT 16" : "SELECT (SELECT count(*) FROM t),coalesce(max(y+CASE WHEN heading THEN ?2 ELSE ?1 END),0) FROM v");
			sqlite3_stmt *st = NULL;
			ok = sqlite3_prepare_v2(c->db, text, -1, &st, NULL) == SQLITE_OK;
			if (ok) {
				sqlite3_bind_int(st, 1, pitch);
				sqlite3_bind_int(st, 2, disc_pitch);
				sqlite3_bind_int(st, 3, offset);
				int rc;
				while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
					if (!pass) {
						out->total = sqlite3_column_int(st, 0);
						int64_t extent = sqlite3_column_int64(st, 1);
						if (extent > INT_MAX || extent < 0) {
							ok = false;
							break;
						}
						out->album.extent = (int)extent;
						continue;
					}
					int n = out->count++;
					out->album.heading[n] = sqlite3_column_int(st, 3) != 0;
					out->album.ordinal[n] = sqlite3_column_int(st, 1);
					out->album.y[n] = sqlite3_column_int(st, 4);
					if (out->album.heading[n])
						out->rows[n].disc = sqlite3_column_int(st, 2);
					else if (sqlite3_column_bytes(st, 0) != sizeof(nd_row_t)) {
						ok = false;
						break;
					} else
						memcpy(&out->rows[n], sqlite3_column_blob(st, 0), sizeof(nd_row_t));
				}
				ok = ok && rc == SQLITE_DONE;
			}
			sqlite3_finalize(st);
		}
		if (c->db) {
			sqlite3_close(c->db);
			c->db = NULL;
		}
		if (lease)
			navidrome_cache_end();
	} else
		ok = false;
	if (!ok || !current(s)) {
		out->count = 0;
		out->error = "nd_cache_storage";
	}
}

bool nd_search_child_position(nd_search_t *s, int level, const char *id, int pitch, int disc_pitch, bool alternate, int *position) {
	if (!s || level < 1 || level > 2 || !current(s))
		return false;
	child_t *c = &s->children[level - 1];
	if (c->source == ND_SEARCH_SYNCED) {
		nd_store_t store = {0};
		bool lease = navidrome_cache_begin(c->library.epoch, c->library.card_epoch);
		bool ok = lease && nd_store_open(&store, c->library.path, c->library.scope, false, false) && nd_store_album_position(&store, c->library.generation, &c->query, id, pitch, disc_pitch, alternate, position);
		nd_store_close(&store);
		if (lease)
			navidrome_cache_end();
		return ok;
	}
	if (c->memory) {
		int groups = 0, disc = 0;
		for (int i = 0; i < c->count; i++)
			if (!i || child_disc(c->memory[i].disc) != child_disc(c->memory[i - 1].disc))
				groups++;
		for (int i = 0; i < c->count; i++) {
			if (!i || child_disc(c->memory[i].disc) != child_disc(c->memory[i - 1].disc))
				disc++;
			if (!strcmp(id, c->memory[i].id)) {
				*position = i * pitch + (alternate && groups > 1 ? disc * disc_pitch : 0);
				return true;
			}
		}
		return false;
	}
	bool lease = navidrome_cache_begin(c->library.epoch, c->library.card_epoch);
	struct stat sb;
	bool ok = lease && !lstat(c->path, &sb) && S_ISREG(sb.st_mode) && sb.st_dev == c->device && sb.st_ino == c->inode && sqlite3_open_v2(c->path, &c->db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK;
	sqlite3_stmt *st = NULL;
	if (ok)
		ok = sqlite3_prepare_v2(c->db, "WITH t AS (SELECT id,row_number() OVER(ORDER BY disc,track,key,id)-1 AS ordinal,dense_rank() OVER(ORDER BY CASE WHEN disc>0 THEN disc ELSE 1 END) AS dr FROM row) SELECT ordinal,dr,(SELECT max(dr) FROM t) FROM t WHERE id=?", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		ok = sqlite3_step(st) == SQLITE_ROW;
		if (ok) {
			int64_t y = sqlite3_column_int64(st, 0) * pitch + (alternate && sqlite3_column_int(st, 2) > 1 ? sqlite3_column_int64(st, 1) * disc_pitch : 0);
			ok = y <= INT_MAX;
			if (ok)
				*position = (int)y;
		}
	}
	sqlite3_finalize(st);
	if (c->db) {
		sqlite3_close(c->db);
		c->db = NULL;
	}
	if (lease)
		navidrome_cache_end();
	return ok;
}
