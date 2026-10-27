#include "navidromediscovery.h"
#include "navidromeapi.h"
#include "navidromecache.h"
#include "navidromedebug.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	int first, count;
	bool known;
	nd_row_t rows[ND_WINDOW];
} window_t;
struct nd_discovery {
	nd_context_t identity;
	nd_discovery_mode_t mode;
	window_t windows[2];
	nd_row_t random[ND_DISCOVERY_RANDOM_MAX];
	int high, random_count, amount, recent, offsets[4];
	uint64_t fingerprints[4];
	bool end, random_ready, child_io;
	unsigned child_epoch;
	nd_album_reader_t *album;
	bool (*current)(void *);
	void *user;
};
static bool current(void *arg) {
	nd_discovery_t *s = arg;
	return s && (!s->child_io || nd_cache_io_current(s->child_epoch)) && (!s->current || s->current(s->user)) && navidrome_search_current(&s->identity, false);
}
nd_discovery_t *nd_discovery_create(nd_discovery_mode_t mode) {
	if ((unsigned)mode >= ND_DISCOVERY_MODES)
		return NULL;
	nd_discovery_t *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	if (!navidrome_context(&s->identity, false)) {
		free(s);
		return NULL;
	}
	nd_wipe(s->identity.password, sizeof(s->identity.password));
	s->mode = mode;
	s->amount = mode == ND_DISCOVERY_RANDOM ? ND_DISCOVERY_RANDOM_MAX : ND_WINDOW;
	return s;
}
void nd_discovery_free(nd_discovery_t *s) {
	if (!s)
		return;
	nd_album_reader_free(s->album);
	nd_wipe(s, sizeof(*s));
	free(s);
}
void nd_discovery_cancel(nd_discovery_t *s, bool (*cb)(void *), void *user) {
	s->current = cb;
	s->user = user;
}
bool nd_discovery_valid(nd_discovery_t *s) { return current(s); }
static bool network_current(void *arg) {
	nd_discovery_t *s = arg;
	return current(s) && navidrome_search_current(&s->identity, true);
}
static const char *page(nd_discovery_t *s, int first, int requested, nd_row_t *rows, int *count) {
	static const char *const types[] = {"newest", "recent", "frequent", "random"};
	nd_context_t ctx;
	if (!current(s) || !navidrome_context(&ctx, true))
		return "nd_reconnect_required";
	if (ctx.profile_epoch != s->identity.profile_epoch) {
		nd_wipe(&ctx, sizeof(ctx));
		return "nd_sync_cancelled";
	}
	s->identity.search_network_epoch = ctx.search_network_epoch;
	char query[128];
	snprintf(query, sizeof(query), "type=%s&size=%d&offset=%d", types[s->mode], requested, first);
	nd_document_t doc;
	const char *error = nd_api_finite_json(ctx.host, ctx.user, ctx.password, "getAlbumList2", query, ND_API_BYTES, network_current, s, &doc);
	if (!error) {
		int obj = json_get(&doc.doc, doc.response, "albumList2");
		int array = json_get(&doc.doc, obj, "album");
		int n = json_len(&doc.doc, array);
		if (!network_current(s))
			error = "nd_sync_cancelled";
		else if (!nd_api_field_unique(&doc.doc, doc.response, "albumList2", true) || obj < 0 || doc.doc.toks[obj].type != JSON_OBJECT || !nd_api_field_unique(&doc.doc, obj, "album", false) || (array >= 0 && doc.doc.toks[array].type != JSON_ARRAY) || n > requested)
			error = "nd_response_error";
		uint64_t fingerprint = 1469598103934665603ULL;
		for (int i = 0; !error && i < n; i++) {
			nd_playrow_t row;
			if (!nd_metadata_row(&doc.doc, json_at(&doc.doc, array, i), ND_ALBUMS, &row)) {
				error = "nd_response_error";
				break;
			}
			rows[i] = row.row;
			for (int j = 0; j < i; j++)
				if (!strcmp(rows[j].id, rows[i].id))
					error = "nd_sync_inconsistent";
			for (const unsigned char *p = (const unsigned char *)row.row.id; *p; p++)
				fingerprint = (fingerprint ^ *p) * 1099511628211ULL;
			fingerprint = (fingerprint ^ 255) * 1099511628211ULL;
		}
		if (!error && n && s->mode != ND_DISCOVERY_RANDOM) {
			for (int i = 0; i < 4; i++)
				if (s->fingerprints[i] == fingerprint && s->offsets[i] != first)
					error = "nd_sync_inconsistent";
			for (int w = 0; w < 2; w++)
				if (s->windows[w].known)
					for (int i = 0; i < n; i++)
						for (int j = 0; j < s->windows[w].count; j++) {
							int old = s->windows[w].first + j;
							bool same = !strcmp(rows[i].id, s->windows[w].rows[j].id);
							if ((same && old != first + i) || (old == first + i && !same))
								error = "nd_sync_inconsistent";
						}
			if (!error) {
				int at = s->recent++ % 4;
				s->fingerprints[at] = fingerprint;
				s->offsets[at] = first;
			}
		}
		if (!error)
			*count = n;
		nd_api_free(&doc);
	}
	if (error && current(s) && (!strcmp(error, "nd_network_error") || !strcmp(error, "nd_tls_error") || !strcmp(error, "nd_auth_error")))
		navidrome_search_failed(&ctx, error);
	nd_wipe(&ctx, sizeof(ctx));
	nd_log("navidrome: discovery mode=%d offset=%d requested=%d accepted=%d result=%s\n", s->mode, first, requested, error ? 0 : *count, error ? error : "ok");
	return error;
}
static const char *adaptive(nd_discovery_t *s, int first, int maximum, nd_row_t *rows, int *count, int *requested) {
	const char *error;
	for (;;) {
		*requested = s->amount < maximum ? s->amount : maximum;
		error = page(s, first, *requested, rows, count);
		if (!error || strcmp(error, "nd_page_large") || *requested <= 1 || !current(s))
			return error;
		s->amount = (*requested + 1) / 2;
	}
}
void nd_discovery_read(nd_discovery_t *s, int first, nd_search_result_t *out) {
	memset(out, 0, sizeof(*out));
	out->kind = ND_ALBUMS;
	out->source = ND_SEARCH_SERVER;
	if (!current(s) || first < 0) {
		out->error = "nd_sync_cancelled";
		return;
	}
	navidrome_library_snapshot(&out->library);
	if (s->mode == ND_DISCOVERY_RANDOM) {
		if (!s->random_ready) {
			int n = 0, requested;
			out->error = adaptive(s, 0, ND_DISCOVERY_RANDOM_MAX, s->random, &n, &requested);
			if (out->error)
				return;
			s->random_count = n;
			s->random_ready = true;
		}
		out->total = s->random_count;
		out->first = first < s->random_count ? first : 0;
		for (int i = out->first; i < s->random_count && out->count < ND_WINDOW; i++)
			out->rows[out->count++] = s->random[i];
		return;
	}
	if (first > s->high)
		first = s->high;
	for (int i = 0; i < 2; i++)
		if (s->windows[i].known && s->windows[i].first == first) {
			out->first = first;
			out->count = s->windows[i].count;
			memcpy(out->rows, s->windows[i].rows, sizeof(out->rows));
			out->total = s->high;
			out->more = !s->end;
			return;
		}
	// Stage one visible window; failed pages never replace accepted windows.
	nd_row_t *rows = calloc(ND_WINDOW, sizeof(*rows));
	if (!rows) {
		out->error = "nd_json_memory";
		return;
	}
	int count = 0;
	bool end = false;
	while (count < ND_WINDOW && !end) {
		int n = 0, requested;
		if (first > INT_MAX - ND_WINDOW) {
			out->error = "nd_search_child_limit";
			break;
		}
		out->error = adaptive(s, first + count, ND_WINDOW - count, rows + count, &n, &requested);
		if (out->error)
			break;
		count += n;
		end = n < requested;
	}
	if (!out->error && current(s)) {
		s->windows[1] = s->windows[0];
		window_t *w = &s->windows[0];
		w->known = true;
		w->first = first;
		w->count = count;
		memcpy(w->rows, rows, sizeof(w->rows));
		if (first <= s->high && first + count >= s->high) {
			s->high = first + count;
			s->end = end;
		}
		out->first = first;
		out->count = count;
		out->total = s->high;
		out->more = !s->end;
		memcpy(out->rows, rows, sizeof(out->rows));
	} else if (!out->error)
		out->error = "nd_sync_cancelled";
	free(rows);
}
void nd_discovery_album_open(nd_discovery_t *s, const char *id, nd_search_result_t *out) {
	nd_album_reader_free(s->album);
	s->album = nd_album_reader_create();
	if (!s->album) {
		memset(out, 0, sizeof(*out));
		out->error = "nd_json_memory";
		return;
	}
	nd_album_reader_cancel(s->album, current, s);
	s->child_epoch = nd_cache_epoch();
	if (!nd_cache_io_begin(s->child_epoch)) {
		memset(out, 0, sizeof(*out));
		out->error = "nd_cache_busy";
		return;
	}
	s->child_io = true;
	nd_album_reader_open(s->album, id, out);
	s->child_io = false;
	nd_cache_io_end();
}
void nd_discovery_album_read(nd_discovery_t *s, int offset, int pitch, int disc_pitch, bool alternate, nd_search_result_t *out) {
	if (!s->album || !nd_cache_io_begin(s->child_epoch)) {
		memset(out, 0, sizeof(*out));
		out->error = "nd_cache_storage";
		return;
	}
	s->child_io = true;
	nd_album_reader_read(s->album, offset, pitch, disc_pitch, alternate, out);
	s->child_io = false;
	nd_cache_io_end();
}
bool nd_discovery_album_position(nd_discovery_t *s, const char *id, int pitch, int disc_pitch, bool alternate, int *position) { return nd_album_reader_position(s->album, id, pitch, disc_pitch, alternate, position); }
