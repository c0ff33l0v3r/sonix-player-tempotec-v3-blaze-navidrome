#include "navidromestars.h"
#include "navidromelibrary.h"
#include "navidromeplayliststore.h"
#include "src/gui/shell/gui.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STAR_SLOTS 32
typedef struct {
	char id[ND_ID_MAX];
	nd_kind_t kind;
	nd_star_state_t state;
	unsigned profile;
	int64_t base;
	bool unsaved;
	uint64_t used;
} star_t;
typedef struct job {
	nd_star_request_t request;
	nd_context_t context;
	nd_star_result_t *result;
	void (*done)(nd_star_result_t *, void *);
	void *user;
} job_t;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static star_t states[STAR_SLOTS];
static uint64_t clock_used;
static bool busy;
static unsigned loading_profile;
static nd_kind_t loading_kind;
static char loading_id[ND_ID_MAX];
static job_t *deferred;

static bool identity(nd_kind_t kind, const char *id) { return kind >= ND_ALBUMS && kind <= ND_ARTISTS && id && *id && strnlen(id, ND_ID_MAX) < ND_ID_MAX; }
static nd_star_state_t state(unsigned profile, nd_kind_t kind, const char *id, bool write, nd_star_state_t value) {
	nd_library_t cache;
	navidrome_favorites_snapshot(&cache);
	int64_t base = kind == ND_ARTISTS ? 0 : cache.base;
	pthread_mutex_lock(&mutex);
	star_t *slot = &states[0];
	for (int i = 0; i < STAR_SLOTS; i++) {
		star_t *s = &states[i];
		if (s->profile == profile && s->kind == kind && !strcmp(s->id, id)) {
			slot = s;
			break;
		}
		if (s->used < slot->used)
			slot = s;
	}
	bool found = slot->profile == profile && slot->kind == kind && !strcmp(slot->id, id);
	nd_star_state_t result = found && slot->base == base ? slot->state : ND_STAR_UNKNOWN;
	if (write) {
		bool keep_unsaved = found && slot->base == base && value != ND_STAR_UNKNOWN && slot->unsaved;
		slot->profile = profile;
		slot->base = base;
		slot->unsaved = keep_unsaved;
		slot->kind = kind;
		snprintf(slot->id, sizeof(slot->id), "%s", id);
		slot->state = value;
	}
	if (found || write)
		slot->used = ++clock_used;
	pthread_mutex_unlock(&mutex);
	return result;
}
nd_star_state_t navidrome_star_state(nd_kind_t kind, const char *id) {
	nd_context_t c;
	if (!identity(kind, id) || !navidrome_context(&c, false))
		return ND_STAR_UNKNOWN;
	unsigned profile = c.profile_epoch;
	nd_wipe(&c, sizeof(c));
	return state(profile, kind, id, false, ND_STAR_UNKNOWN);
}
static void unsaved(unsigned profile, nd_kind_t kind, const char *id, bool value) {
	pthread_mutex_lock(&mutex);
	for (int i = 0; i < STAR_SLOTS; i++)
		if (states[i].profile == profile && states[i].kind == kind && !strcmp(states[i].id, id))
			states[i].unsaved = value;
	pthread_mutex_unlock(&mutex);
}
bool navidrome_star_retry(nd_kind_t kind, const char *id) {
	nd_context_t c;
	if (!navidrome_context(&c, false))
		return false;
	nd_library_t cache;
	navidrome_favorites_snapshot(&cache);
	bool retry = false;
	pthread_mutex_lock(&mutex);
	for (int i = 0; i < STAR_SLOTS; i++)
		if (states[i].profile == c.profile_epoch && states[i].kind == kind && states[i].base == (kind == ND_ARTISTS ? 0 : cache.base) && !strcmp(states[i].id, id))
			retry = states[i].unsaved;
	pthread_mutex_unlock(&mutex);
	nd_wipe(&c, sizeof(c));
	return retry;
}

bool navidrome_star_loading(nd_kind_t kind, const char *id) {
	nd_context_t c;
	if (!navidrome_context(&c, false))
		return false;
	pthread_mutex_lock(&mutex);
	bool active = loading_profile == c.profile_epoch && loading_kind == kind && id && loading_id[0] && !strcmp(id, loading_id);
	pthread_mutex_unlock(&mutex);
	nd_wipe(&c, sizeof(c));
	return active;
}
static void loading(const nd_context_t *c, nd_kind_t kind, const char *id) {
	pthread_mutex_lock(&mutex);
	loading_profile = c->profile_epoch;
	loading_kind = kind;
	snprintf(loading_id, sizeof(loading_id), "%s", id ? id : "");
	pthread_mutex_unlock(&mutex);
}
bool navidrome_star_busy(void) {
	pthread_mutex_lock(&mutex);
	bool result = busy || deferred;
	pthread_mutex_unlock(&mutex);
	return result;
}
bool navidrome_star_current(const nd_context_t *context) { return navidrome_context_current(context, false) && navidrome_playback_current(context, false); }
static bool request_current(void *user) {
	navidrome_poll_network();
	return navidrome_star_current(user) && navidrome_context_current(user, true);
}
static const char *read_state(const nd_context_t *c, nd_kind_t kind, const char *id, nd_star_state_t *out) {
	char encoded[ND_ID_MAX * 3], query[ND_ID_MAX * 3 + 8];
	if (!http_url_encode(id, encoded, sizeof(encoded)))
		return "nd_input_long";
	snprintf(query, sizeof(query), "id=%s", encoded);
	const char *methods[] = {"getAlbum", "getSong", "getArtist"};
	const char *objects[] = {"album", "song", "artist"};
	nd_document_t doc;
	const char *error = nd_api_finite_json(c->host, c->user, c->password, methods[kind], query, ND_API_BYTES, request_current, (void *)c, &doc);
	if (error)
		return error;
	int object = json_get(&doc.doc, doc.response, objects[kind]);
	int token = json_get(&doc.doc, object, "id");
	char actual[ND_ID_MAX];
	bool starred = false;
	bool valid = token >= 0 && doc.doc.toks[token].end - doc.doc.toks[token].start < ND_ID_MAX && object >= 0 && doc.doc.toks[object].type == JSON_OBJECT && token >= 0 && doc.doc.toks[token].type == JSON_STRING;
	if (valid)
		for (int i = doc.doc.toks[token].start; i + 6 <= doc.doc.toks[token].end; i++)
			if (!memcmp(doc.doc.text + i, "\\u0000", 6))
				valid = false;
	valid = valid && nd_api_field_unique(&doc.doc, doc.response, objects[kind], true) && nd_api_field_unique(&doc.doc, object, "id", true) && nd_api_field_unique(&doc.doc, object, "starred", false) && json_str(&doc.doc, token, actual, sizeof(actual)) && !strcmp(actual, id) && nd_metadata_starred(&doc.doc, object, &starred);
	nd_api_free(&doc);
	if (!valid)
		return "nd_response_error";
	*out = starred ? ND_STAR_SET : ND_STAR_CLEAR;
	return NULL;
}
static const char *one(const nd_context_t *c, nd_kind_t kind, const char *id, nd_star_action_t action, bool *uncertain) {
	if (!identity(kind, id) || !request_current((void *)c))
		return "nd_favorites_stale";
	nd_star_state_t previous = state(c->profile_epoch, kind, id, false, ND_STAR_UNKNOWN);
	if (previous == ND_STAR_UNKNOWN && navidrome_cache_begin(c->epoch, c->card_epoch)) {
		nd_library_t cache;
		navidrome_favorites_snapshot(&cache);
		nd_store_t store = {0};
		bool starred = false;
		if (cache.ready && nd_store_open_source(&store, cache.path, cache.scope, false, false, true) && nd_store_annotation(&store, cache.generation, kind, id, &starred))
			previous = starred ? ND_STAR_SET : ND_STAR_CLEAR;
		nd_store_close(&store);
		navidrome_cache_end();
	}
	if (previous == ND_STAR_PENDING)
		previous = ND_STAR_UNKNOWN;
	if (previous == ND_STAR_UNCERTAIN && action != ND_STAR_CHECK) {
		*uncertain = true;
		return "nd_favorites_uncertain";
	}
	if (action == ND_STAR_CHECK || (action == ND_STAR_TOGGLE && previous == ND_STAR_UNKNOWN)) {
		const char *error = read_state(c, kind, id, &previous);
		if (error || !request_current((void *)c)) {
			state(c->profile_epoch, kind, id, true, ND_STAR_UNKNOWN);
			return error ? error : "nd_favorites_stale";
		}
		state(c->profile_epoch, kind, id, true, previous);
		if (action == ND_STAR_CHECK)
			return NULL;
	}
	bool add = action == ND_STAR_ADD || (action == ND_STAR_TOGGLE && previous != ND_STAR_SET);
	char encoded[ND_ID_MAX * 3], query[ND_ID_MAX * 3 + 16];
	if (!http_url_encode(id, encoded, sizeof(encoded)))
		return "nd_input_long";
	const char *parameters[] = {"albumId", "id", "artistId"};
	snprintf(query, sizeof(query), "%s=%s", parameters[kind], encoded);
	state(c->profile_epoch, kind, id, true, ND_STAR_PENDING);
	nd_document_t doc;
	const char *error = nd_api_finite_json(c->host, c->user, c->password, add ? "star" : "unstar", query, 16384, request_current, (void *)c, &doc);
	if (!error)
		nd_api_free(&doc);
	bool rejected = error && (!strcmp(error, "nd_api_error") || !strcmp(error, "nd_auth_error"));
	*uncertain = error && !rejected;
	if (!request_current((void *)c)) {
		state(c->profile_epoch, kind, id, true, ND_STAR_UNKNOWN);
		return "nd_favorites_stale";
	}
	state(c->profile_epoch, kind, id, true, error ? rejected ? previous : ND_STAR_UNCERTAIN : add ? ND_STAR_SET : ND_STAR_CLEAR);
	return *uncertain ? "nd_favorites_uncertain" : error;
}
// Read a fixed window of captured selections, then close all storage before HTTP.
static const char *selection_window(const nd_star_request_t *q, const nd_context_t *c, int after, char ids[ND_WINDOW][ND_ID_MAX], int ordinals[ND_WINDOW], int *count, int *total) {
	*count = *total = 0;
	if (!request_current((void *)c) || !navidrome_cache_begin(c->epoch, c->card_epoch))
		return "nd_favorites_stale";
	char path[1200];
	nd_playlist_store_t store = {0};
	bool ok = q->library.ready && q->library.epoch == c->epoch && q->library.card_epoch == c->card_epoch && nd_pl_path(&q->library, path) && nd_pl_open(&store, path, q->library.scope, false);
	sqlite3_stmt *st = NULL;
	if (ok)
		ok = nd_pl_prepare(&store, &st, "SELECT count(*) FROM selection WHERE session=? AND kind=?");
	if (ok) {
		sqlite3_bind_int64(st, 1, q->session);
		sqlite3_bind_int(st, 2, q->kind);
		ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) <= INT_MAX;
		if (ok)
			*total = sqlite3_column_int(st, 0);
	}
	sqlite3_finalize(st);
	st = NULL;
	if (ok)
		ok = nd_pl_prepare(&store, &st, "SELECT ordinal,track FROM selection WHERE session=? AND kind=? AND ordinal>? ORDER BY ordinal LIMIT 16");
	if (ok) {
		sqlite3_bind_int64(st, 1, q->session);
		sqlite3_bind_int(st, 2, q->kind);
		sqlite3_bind_int(st, 3, after);
		int rc;
		while ((rc = sqlite3_step(st)) == SQLITE_ROW && *count < ND_WINDOW) {
			const char *id = (const char *)sqlite3_column_text(st, 1);
			if (!identity(q->kind, id)) {
				ok = false;
				break;
			}
			ordinals[*count] = sqlite3_column_int(st, 0);
			snprintf(ids[(*count)++], ND_ID_MAX, "%s", id);
		}
		ok = ok && rc == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	nd_pl_close(&store);
	navidrome_cache_end();
	return ok ? NULL : "nd_pl_storage_error";
}
// All helpers below run on the serialized star worker. Leases cover only SQL,
// and publication advances Favorites revisions without changing the action epoch.
static bool load_record(const nd_star_request_t *q, const nd_context_t *c, nd_kind_t kind, const char *id, nd_playrow_t *row, nd_library_t *source) {
	memset(row, 0, sizeof(*row));
	memset(source, 0, sizeof(*source));
	nd_library_t candidates[3];
	navidrome_favorites_snapshot(&candidates[0]);
	candidates[1] = q->library;
	navidrome_library_snapshot(&candidates[2]);
	bool found = false;
	if (navidrome_cache_begin(c->epoch, c->card_epoch)) {
		for (int i = 0; i < 3 && !found; i++) {
			nd_library_t *lib = &candidates[i];
			nd_store_t store = {0};
			if (lib->ready && lib->epoch == c->epoch && lib->card_epoch == c->card_epoch && !strcmp(lib->scope, candidates[0].scope) && nd_store_open_source(&store, lib->path, lib->scope, false, false, lib->favorites)) {
				found = nd_store_metadata(&store, lib->generation, kind, id, row);
				if (found)
					*source = *lib;
			}
			nd_store_close(&store);
		}
		navidrome_cache_end();
	}
	if (!found && !strcmp(q->metadata.row.id, id)) {
		*row = q->metadata;
		found = true;
	}
	if (!found) {
		snprintf(row->row.id, sizeof(row->row.id), "%s", id);
		snprintf(row->row.title, sizeof(row->row.title), "%s", id);
	}
	return found;
}
static bool local_open(const nd_context_t *c, nd_store_t *store, nd_library_t *cache) {
	navidrome_favorites_snapshot(cache);
	return cache->writable && !cache->corrupt && cache->path[0] && cache->scope[0] && navidrome_star_current(c) && nd_cache_open_sync(cache, store) && nd_store_local_begin(store);
}
static bool local_commit(const nd_context_t *c, nd_store_t *store, nd_library_t *cache) {
	bool ok = nd_store_local_activate(store) && navidrome_context_commit(c, store->db) && nd_store_status(store, cache) && navidrome_favorites_adopt(c, cache);
	if (ok)
		nd_store_cleanup(store);
	return ok;
}
static const char *save_record(const nd_context_t *c, nd_kind_t kind, nd_playrow_t *row, bool starred, int details, const nd_library_t *source) {
	if (!navidrome_cache_begin(c->epoch, c->card_epoch))
		return "nd_favorites_unsaved";
	nd_library_t cache;
	nd_store_t store = {0}, old = {0};
	bool ok = local_open(c, &store, &cache);
	if (ok && kind == ND_ALBUMS && starred && source && source->ready && details == 0) {
		ok = nd_store_open_source(&old, source->path, source->scope, false, false, source->favorites);
		nd_query_t query = {.kind = ND_TRACKS, .favorites = source->favorites};
		snprintf(query.filter, sizeof(query.filter), "%s", row->row.id);
		int offset = 0, count = 0, total = 0;
		do {
			nd_row_t rows[ND_WINDOW];
			ok = ok && nd_store_query(&old, source->generation, &query, offset, rows, &count, &total);
			for (int i = 0; ok && i < count; i++) {
				nd_playrow_t track = {.row = rows[i]};
				snprintf(track.album_id, sizeof(track.album_id), "%s", row->row.id);
				bool star = track.row.favorite;
				if (cache.ready)
					nd_store_annotation(&store, cache.generation, ND_TRACKS, track.row.id, &star);
				ok = nd_store_local_put(&store, ND_TRACKS, &track, star, 0);
			}
			offset += count;
		} while (ok && offset < total);
	}
	if (ok)
		ok = nd_store_local_put(&store, kind, row, starred, details) && local_commit(c, &store, &cache);
	if (!ok && store.db)
		sqlite3_exec(store.db, "ROLLBACK", NULL, NULL, NULL);
	nd_store_close(&old);
	nd_store_close(&store);
	navidrome_cache_end();
	return ok ? NULL : "nd_favorites_unsaved";
}
static const char *get_metadata(const nd_context_t *c, nd_kind_t kind, const char *id, nd_document_t *doc, int *object) {
	char encoded[ND_ID_MAX * 3], query[ND_ID_MAX * 3 + 8];
	if (!http_url_encode(id, encoded, sizeof(encoded)))
		return "nd_input_long";
	snprintf(query, sizeof(query), "id=%s", encoded);
	const char *name = kind == ND_ALBUMS ? "album" : "song";
	const char *error = nd_api_finite_json(c->host, c->user, c->password, kind == ND_ALBUMS ? "getAlbum" : "getSong", query, ND_API_BYTES, request_current, (void *)c, doc);
	if (error)
		return error;
	*object = json_get(&doc->doc, doc->response, name);
	nd_playrow_t row;
	if (!nd_api_field_unique(&doc->doc, doc->response, name, true) || !nd_metadata_row(&doc->doc, *object, kind, &row) || strcmp(row.row.id, id)) {
		nd_api_free(doc);
		return "nd_response_error";
	}
	return NULL;
}
static const char *save_album_document(const nd_context_t *c, const char *id, const nd_document_t *doc, int object, int64_t expected_revision) {
	const json_doc_t *d = &doc->doc;
	nd_playrow_t album;
	if (!nd_metadata_row(d, object, ND_ALBUMS, &album) || strcmp(album.row.id, id) || !nd_api_field_unique(d, object, "song", false))
		return "nd_favorites_metadata_error";
	int array = json_get(d, object, "song"), count = json_len(d, array), token = json_get(d, object, "songCount");
	long expected = json_obj_long(d, object, "songCount", count);
	if (token >= 0)
		for (int i = d->toks[token].start; i < d->toks[token].end; i++)
			if (d->text[i] < '0' || d->text[i] > '9')
				return "nd_favorites_metadata_error";
	if (expected < 0 || expected > INT_MAX || (token >= 0 && d->toks[token].type != JSON_NUMBER) || count != expected || (array >= 0 && d->toks[array].type != JSON_ARRAY) || (array < 0 && expected != 0))
		return "nd_favorites_metadata_error";
	if (!navidrome_cache_begin(c->epoch, c->card_epoch))
		return "nd_favorites_unsaved";
	nd_library_t cache;
	nd_store_t store = {0};
	bool ok = local_open(c, &store, &cache), star = false;
	ok = ok && cache.ready && cache.generation == expected_revision && nd_store_annotation(&store, cache.generation, ND_ALBUMS, id, &star) && star;
	for (int i = 0; ok && i < count; i++) {
		nd_playrow_t track;
		ok = nd_metadata_row(d, json_at(d, array, i), ND_TRACKS, &track) && !strcmp(track.album_id, id) && nd_store_local_put(&store, ND_TRACKS, &track, track.row.favorite, 0);
	}
	if (ok)
		ok = nd_store_local_put(&store, ND_ALBUMS, &album, true, 0) && local_commit(c, &store, &cache);
	if (!ok && store.db)
		sqlite3_exec(store.db, "ROLLBACK", NULL, NULL, NULL);
	nd_store_close(&store);
	navidrome_cache_end();
	return ok ? NULL : "nd_favorites_metadata_error";
}
static const char *cache_change(const nd_star_request_t *q, const nd_context_t *c, nd_kind_t kind, const char *id, bool starred) {
	nd_playrow_t row;
	nd_library_t source;
	bool found = load_record(q, c, kind, id, &row, &source);
	int details = starred && ((kind == ND_ALBUMS && (!source.ready || row.row.details)) || (kind == ND_TRACKS && (!found || !row.album_id[0]))) ? 1 : 0;
	const char *error = save_record(c, kind, &row, starred, details, &source);
	if (error || !starred || kind == ND_ARTISTS)
		return error;
	if (kind == ND_TRACKS && found && row.album_id[0])
		return NULL;
	if (kind == ND_ALBUMS && !details)
		return NULL;
	loading(c, kind, id);
	nd_library_t published;
	navidrome_favorites_snapshot(&published);
	nd_document_t doc;
	int object = -1;
	error = get_metadata(c, kind, id, &doc, &object);
	if (!error) {
		if (kind == ND_ALBUMS)
			error = save_album_document(c, id, &doc, object, published.generation);
		else if (!nd_metadata_row(&doc.doc, object, kind, &row))
			error = "nd_response_error";
		else
			error = save_record(c, kind, &row, true, 0, NULL);
		nd_api_free(&doc);
	}
	loading(c, kind, NULL);
	if (error) {
		// Membership was already committed; only mark details unavailable.
		nd_library_t current;
		navidrome_favorites_snapshot(&current);
		if (current.generation == published.generation)
			save_record(c, kind, &row, true, 2, NULL);
		return "nd_favorites_metadata_error";
	}
	return error ? "nd_favorites_metadata_error" : NULL;
}
static const char *execute_item(const nd_star_request_t *q, const nd_context_t *c, const char *id, nd_star_action_t action, nd_star_result_t *out) {
	const char *error = NULL;
	if (action != ND_STAR_RETRY)
		error = one(c, q->kind, id, action, &out->uncertain);
	nd_star_state_t confirmed = state(c->profile_epoch, q->kind, id, false, ND_STAR_UNKNOWN);
	if (action == ND_STAR_RETRY && confirmed == ND_STAR_UNKNOWN && navidrome_cache_begin(c->epoch, c->card_epoch)) {
		nd_library_t cache;
		navidrome_favorites_snapshot(&cache);
		nd_store_t store = {0};
		bool star = false;
		if (cache.ready && nd_store_open_source(&store, cache.path, cache.scope, false, false, true) && nd_store_annotation(&store, cache.generation, q->kind, id, &star)) {
			confirmed = star ? ND_STAR_SET : ND_STAR_CLEAR;
			state(c->profile_epoch, q->kind, id, true, confirmed);
		}
		nd_store_close(&store);
		navidrome_cache_end();
	}
	if (error) {
		out->failed += !out->uncertain;
		return error;
	}
	if (confirmed != ND_STAR_SET && confirmed != ND_STAR_CLEAR)
		return "nd_favorites_stale";
	if (action != ND_STAR_RETRY)
		out->completed++;
	error = cache_change(q, c, q->kind, id, confirmed == ND_STAR_SET);
	unsaved(c->profile_epoch, q->kind, id, error != NULL);
	if (error)
		out->cache_failed = true;
	else
		out->saved++;
	return error;
}

void nd_star_execute(const nd_star_request_t *q, const nd_context_t *c, nd_star_result_t *out) {
	memset(out, 0, sizeof(*out));
	out->request = *q;
	out->context = *c;
	nd_wipe(out->context.password, sizeof(out->context.password));
	if (q->bound_profile && q->profile_epoch != c->profile_epoch) {
		out->error = "nd_favorites_stale";
		return;
	}
	if (q->action != ND_STAR_SELECTED) {
		out->total = 1;
		nd_star_action_t action = q->action;
		if (action == ND_STAR_TOGGLE && state(c->profile_epoch, q->kind, q->id, false, ND_STAR_UNKNOWN) == ND_STAR_UNCERTAIN)
			action = ND_STAR_CHECK;
		out->request.action = action;
		out->error = execute_item(q, c, q->id, action, out);
		return;
	}
	if (q->kind != ND_TRACKS && q->kind != ND_ALBUMS) {
		out->error = "nd_favorites_stale";
		return;
	}
	int last = -1, count, total;
	do {
		char ids[ND_WINDOW][ND_ID_MAX];
		int ordinals[ND_WINDOW];
		out->error = selection_window(q, c, last, ids, ordinals, &count, &total);
		if (out->error)
			return;
		if (out->total && out->total != total) {
			out->error = "nd_favorites_stale";
			return;
		}
		out->total = total;
		for (int i = 0; i < count; i++) {
			const char *error = execute_item(q, c, ids[i], ND_STAR_ADD, out);
			last = ordinals[i];
			if (error) {
				out->error = error;
				return; // No replay; submitted successes remain successful.
			}
		}
	} while (count);
	if (!out->total)
		out->error = "nd_favorites_stale";
}
static void delivered(void *data) {
	job_t *job = data;
	nd_star_result_t *result = job->result;
	if (!navidrome_star_current(&result->context))
		result->error = "nd_favorites_stale";
	if (job->done)
		job->done(result, job->user);
	else
		free(result);
	free(job);
}
void navidrome_star_poll(void) {
	pthread_mutex_lock(&mutex);
	job_t *job = deferred;
	deferred = NULL;
	pthread_mutex_unlock(&mutex);
	if (job)
		delivered(job);
}
static void *run(void *data) {
	job_t *job = data;
	nd_star_execute(&job->request, &job->context, job->result);
	nd_wipe(&job->context, sizeof(job->context));
	// Keep service admission until delivery is posted or fallback is retained.
	if (!gui_post(delivered, job)) {
		pthread_mutex_lock(&mutex);
		deferred = job;
		pthread_mutex_unlock(&mutex);
	}
	pthread_mutex_lock(&mutex);
	busy = false;
	pthread_mutex_unlock(&mutex);
	navidrome_external_end();
	return NULL;
}
bool navidrome_star_submit(const nd_star_request_t *q, void (*done)(nd_star_result_t *, void *), void *user) {
	navidrome_star_poll();
	if (!q || q->action < ND_STAR_ADD || q->action > ND_STAR_RETRY || (q->action != ND_STAR_SELECTED && !identity(q->kind, q->id)))
		return false;
	job_t *job = calloc(1, sizeof(*job));
	if (!job)
		return false;
	job->result = calloc(1, sizeof(*job->result));
	if (!job->result) {
		free(job);
		return false;
	}
	navidrome_poll_network();
	bool network = q->action != ND_STAR_RETRY;
	bool ok = navidrome_context(&job->context, network);
	ok = ok && (!q->bound_profile || q->profile_epoch == job->context.profile_epoch);
	if (q->library.ready)
		ok = ok && q->library.epoch == job->context.epoch && q->library.card_epoch == job->context.card_epoch;
	pthread_mutex_lock(&mutex);
	ok = ok && !busy && !deferred && navidrome_external_begin(&job->context, network);
	if (!ok) {
		pthread_mutex_unlock(&mutex);
		free(job->result);
		nd_wipe(job, sizeof(*job));
		free(job);
		return false;
	}
	job->request = *q;
	job->done = done;
	job->user = user;
	busy = true;
	pthread_t worker;
	pthread_attr_t attr;
	int failed = pthread_attr_init(&attr);
	if (!failed) {
		failed = pthread_attr_setstacksize(&attr, 256 * 1024);
		if (!failed)
			failed = pthread_create(&worker, &attr, run, job);
		pthread_attr_destroy(&attr);
	}
	if (failed) {
		busy = false;
		navidrome_external_end();
		pthread_mutex_unlock(&mutex);
		free(job->result);
		nd_wipe(job, sizeof(*job));
		free(job);
		return false;
	}
	pthread_detach(worker);
	pthread_mutex_unlock(&mutex);
	return true;
}
