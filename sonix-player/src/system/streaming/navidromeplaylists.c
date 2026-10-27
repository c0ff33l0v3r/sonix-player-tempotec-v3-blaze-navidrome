#include "navidromeplaylists.h"
#include "src/gui/shell/gui.h"
#include "src/system/net/http.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Includes conservative old/new token arrays during realloc; bodies/forms are released between phases.
_Static_assert(2 * (ND_PL_BYTES + 1) + ND_PL_TOKENS * sizeof(json_tok_t) * 3 / 2 + 8192 < 8 * 1024 * 1024, "playlist response/parser/form budget");

static bool network_action(nd_pl_action_t a) { return a >= NDPL_CATALOGUE && a != NDPL_CATALOGUE_WINDOW && a != NDPL_PULL_ACCEPT; }
static bool readonly_action(nd_pl_action_t a) { return a == NDPL_LIST || a == NDPL_ENTRIES || a == NDPL_CATALOGUE_WINDOW; }
static bool commit(nd_playlist_store_t *s, const nd_context_t *c) {
	if (navidrome_context_commit(c, s->db))
		return true;
	s->error = "nd_pl_stale";
	return false;
}
static bool statement_id(nd_playlist_store_t *s, const char *sql, int64_t id) {
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, sql))
		return false;
	sqlite3_bind_int64(st, 1, id);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}
static int selection_count(nd_playlist_store_t *s, int64_t session) {
	sqlite3_stmt *st = NULL;
	int count = -1;
	if (nd_pl_prepare(s, &st, "SELECT count(*) FROM selection WHERE session=?")) {
		sqlite3_bind_int64(st, 1, session);
		if (sqlite3_step(st) == SQLITE_ROW)
			count = sqlite3_column_int(st, 0);
	}
	sqlite3_finalize(st);
	return count;
}
static bool toggle(nd_playlist_store_t *s, const nd_pl_request_t *q) {
	if (!nd_pl_sql(s, "BEGIN IMMEDIATE"))
		return false;
	if (q->action == NDPL_SELECT_ONLY && !nd_pl_sql(s, "DELETE FROM selection"))
		return false;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, q->occurrence ? "DELETE FROM selection WHERE session=? AND ordinal=?" : "DELETE FROM selection WHERE session=? AND kind=? AND track=?"))
		return false;
	sqlite3_bind_int64(st, 1, q->session);
	sqlite3_bind_int(st, 2, q->occurrence ? q->ordinal : (int)q->kind);
	if (!q->occurrence)
		nd_pl_bind(st, 3, q->track);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	int removed = sqlite3_changes(s->db);
	sqlite3_finalize(st);
	if (ok && !removed) {
		if (!nd_pl_prepare(s, &st, "INSERT INTO selection(session,ordinal,kind,track,occurrence) VALUES(?1,CASE WHEN EXISTS(SELECT 1 FROM selection WHERE session=?1 AND ordinal=?2) THEN (SELECT coalesce(max(ordinal),-1)+1 FROM selection WHERE session=?1) ELSE ?2 END,?3,?4,?5)"))
			return false;
		sqlite3_bind_int64(st, 1, q->session);
		sqlite3_bind_int(st, 2, q->ordinal);
		sqlite3_bind_int(st, 3, q->kind);
		nd_pl_bind(st, 4, q->track);
		sqlite3_bind_int64(st, 5, q->occurrence);
		ok = sqlite3_step(st) == SQLITE_DONE;
		sqlite3_finalize(st);
	}
	return ok;
}
static bool add_selected(nd_playlist_store_t *s, const nd_pl_request_t *q, const nd_context_t *c, const nd_library_t *lib, nd_pl_result_t *out) {
	if (q->direct) {
		if (q->source.profile != c->profile_epoch || q->source.card != c->card_epoch ||
			!memchr(q->source.scope, 0, sizeof(q->source.scope)) || strcmp(q->source.scope, lib->scope) || !q->source.row.id[0] ||
			!memchr(q->source.row.id, 0, sizeof(q->source.row.id)) ||
			!memchr(q->source.row.title, 0, sizeof(q->source.row.title)) ||
			!memchr(q->source.row.artist, 0, sizeof(q->source.row.artist)) ||
			!memchr(q->source.row.cover, 0, sizeof(q->source.row.cover))) {
			s->error = "nd_pl_stale";
			return false;
		}
		return nd_pl_begin(s, q->id, q->revision) &&
			nd_pl_append(s, q->id, &q->source.row, true, &out->added, &out->skipped) &&
			nd_pl_finish(s, q->id, out->added > 0) && commit(s, c);
	}
	if (!lib->ready || q->generation != lib->generation) {
		s->error = "nd_pl_stale";
		return false;
	}
	nd_store_t source = {0};
	if (!nd_store_open_source(&source, lib->path, lib->scope, false, false, lib->favorites)) {
		nd_store_close(&source);
		return false;
	}
	bool ok = nd_pl_begin(s, q->id, q->revision);
	sqlite3_stmt *st = NULL;
	if (ok)
		ok = nd_pl_prepare(s, &st, "SELECT kind,track FROM selection WHERE session=? ORDER BY ordinal");
	if (ok)
		sqlite3_bind_int64(st, 1, q->session);
	int rc = SQLITE_DONE;
	while (ok && (rc = sqlite3_step(st)) == SQLITE_ROW) {
		char id[ND_ID_MAX];
		const char *value = (const char *)sqlite3_column_text(st, 1);
		snprintf(id, sizeof(id), "%s", value ? value : "");
		if (sqlite3_column_int(st, 0) == ND_TRACKS) {
			nd_row_t row;
			ok = nd_store_track(&source, lib->generation, id, &row) && nd_pl_append(s, q->id, &row, true, &out->added, &out->skipped);
		} else if (sqlite3_column_int(st, 0) == ND_ALBUMS) {
			nd_query_t query = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS, .favorites = lib->favorites};
			snprintf(query.filter, sizeof(query.filter), "%s", id);
			ok = nd_store_contains(&source, lib->generation, ND_ALBUMS, id);
			int first = 0, total = 0, count = 0;
			do {
				nd_row_t rows[ND_WINDOW];
				if (ok)
					ok = navidrome_context_current(c, false) && nd_store_query(&source, lib->generation, &query, first, rows, &count, &total);
				for (int i = 0; i < count && ok; i++)
					ok = nd_pl_append(s, q->id, &rows[i], true, &out->added, &out->skipped);
				first += count;
			} while (ok && count && first < total);
		} else
			ok = false;
	}
	sqlite3_finalize(st);
	nd_store_close(&source);
	if (ok)
		ok = rc == SQLITE_DONE && nd_pl_finish(s, q->id, out->added > 0) && statement_id(s, "DELETE FROM selection WHERE session=?", q->session) && commit(s, c);
	return ok;
}
static const char *api(const nd_context_t *c, const char *method, const char *query, const char *form, nd_document_t *doc) {
	if (!navidrome_context_current(c, true))
		return "nd_pl_stale";
	return nd_api_playlist(c->host, c->user, c->password, method, query, form, doc);
}
// No card handle/lease survives a network wait. Card export can drain immediately.
static const char *api_store(nd_playlist_store_t *s, const nd_context_t *c, const char *method, const char *query, const char *form, nd_document_t *doc) {
	char path[1200], scope[33];
	snprintf(path, sizeof(path), "%s", s->path);
	snprintf(scope, sizeof(scope), "%s", s->scope);
	nd_pl_close(s);
	if (s->leased)
		navidrome_cache_end();
	s->leased = false;
	const char *error = api(c, method, query, form, doc);
	navidrome_poll_network();
	if (!error && !navidrome_context_current(c, true)) {
		nd_api_free(doc);
		error = "nd_pl_stale";
	}
	bool leased = navidrome_cache_begin(c->epoch, c->card_epoch);
	bool opened = leased && nd_pl_open(s, path, scope, true);
	s->leased = leased;
	if (!opened) {
		if (!error)
			nd_api_free(doc);
		return "nd_pl_stale";
	}
	return error;
}
static unsigned post_epoch, post_network;
static char post_host[ND_HOST_MAX];
static bool post_supported;
static const char *capability(nd_playlist_store_t *s, const nd_context_t *c) {
	if (post_supported && post_epoch == c->epoch && post_network == c->network_epoch && !strcmp(post_host, c->host))
		return NULL;
	nd_document_t doc;
	const char *error = api_store(s, c, "getOpenSubsonicExtensions", NULL, NULL, &doc);
	if (error)
		return error;
	int arr = json_get(&doc.doc, doc.response, "openSubsonicExtensions");
	bool found = false;
	for (int t = arr + 1; arr >= 0 && t < doc.doc.count && doc.doc.toks[t].start < doc.doc.toks[arr].end; t++) {
		if (doc.doc.toks[t].parent != arr)
			continue;
		char name[64];
		json_obj_str(&doc.doc, t, "name", name, sizeof(name));
		int versions = json_get(&doc.doc, t, "versions");
		if (!strcmp(name, "formPost"))
			for (int i = 0; i < json_len(&doc.doc, versions); i++)
				if (json_long(&doc.doc, json_at(&doc.doc, versions, i), 0) == 1)
					found = true;
	}
	nd_api_free(&doc);
	if (!found)
		return "nd_pl_post_unsupported";
	post_supported = true;
	post_epoch = c->epoch;
	post_network = c->network_epoch;
	snprintf(post_host, sizeof(post_host), "%s", c->host);
	return NULL;
}
static bool fetch(nd_playlist_store_t *s, const nd_context_t *c, const char *remote, int64_t *candidate, nd_playlist_t *p) {
	char encoded[ND_ID_MAX * 3], query[ND_ID_MAX * 3 + 8];
	http_url_encode(remote, encoded, sizeof(encoded));
	snprintf(query, sizeof(query), "id=%s", encoded);
	nd_document_t doc;
	const char *error = api_store(s, c, "getPlaylist", query, NULL, &doc);
	if (error) {
		s->error = error;
		return false;
	}
	bool leased = navidrome_cache_begin(c->epoch, c->card_epoch);
	bool ok = leased && nd_pl_sql(s, "BEGIN IMMEDIATE") && nd_pl_decode(s, &doc, candidate, p) && !strcmp(p->remote, remote) && commit(s, c);
	if (!ok && !s->error)
		s->error = "nd_response_error";
	if (!ok)
		nd_pl_sql(s, "ROLLBACK");
	if (leased)
		navidrome_cache_end();
	nd_api_free(&doc);
	return ok;
}
static bool install_candidate(nd_playlist_store_t *s, const nd_context_t *c, const nd_pl_request_t *q, int64_t candidate, const nd_playlist_t *remote, bool importing, nd_pl_result_t *out) {
	bool leased = navidrome_cache_begin(c->epoch, c->card_epoch), ok = leased;
	int64_t id = q->id;
	if (ok)
		ok = importing ? nd_pl_sql(s, "BEGIN IMMEDIATE") && nd_pl_create(s, remote->name, &id) : nd_pl_begin(s, id, q->revision);
	if (ok && !importing)
		ok = statement_id(s, "DELETE FROM entry WHERE p=?", id);
	sqlite3_stmt *st = NULL;
	if (ok)
		ok = nd_pl_prepare(s, &st, "INSERT INTO entry(p,ordinal,track,title,artist,cover,duration) SELECT ?,ordinal,track,title,artist,cover,duration FROM entry WHERE p=? ORDER BY ordinal");
	if (ok) {
		sqlite3_bind_int64(st, 1, id);
		sqlite3_bind_int64(st, 2, candidate);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	st = NULL;
	if (ok)
		ok = nd_pl_prepare(s, &st, "UPDATE playlist SET name=? WHERE id=?");
	if (ok) {
		nd_pl_bind(st, 1, remote->name);
		sqlite3_bind_int64(st, 2, id);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	if (ok)
		ok = nd_pl_baseline(s, id, remote->remote, remote->baseline, remote->readonly, remote->changed) && nd_pl_finish(s, id, !importing) && statement_id(s, "DELETE FROM playlist WHERE id=?", candidate) && commit(s, c);
	if (ok)
		ok = nd_pl_get(s, id, &out->playlist);
	if (!ok)
		nd_pl_sql(s, "ROLLBACK");
	if (leased)
		navidrome_cache_end();
	return ok;
}
static bool catalogue(nd_playlist_store_t *s, const nd_context_t *c) {
	nd_document_t doc;
	const char *error = api_store(s, c, "getPlaylists", NULL, NULL, &doc);
	if (error) {
		s->error = error;
		return false;
	}
	int obj = json_get(&doc.doc, doc.response, "playlists"), arr = json_get(&doc.doc, obj, "playlist");
	bool leased = navidrome_cache_begin(c->epoch, c->card_epoch);
	bool ok = leased && obj >= 0 && doc.doc.toks[obj].type == JSON_OBJECT && (arr < 0 || doc.doc.toks[arr].type == JSON_ARRAY) && nd_pl_sql(s, "BEGIN IMMEDIATE; DELETE FROM catalogue;");
	sqlite3_stmt *st = NULL;
	if (ok)
		ok = nd_pl_prepare(s, &st, "INSERT INTO catalogue VALUES(?,?,?,?)");
	int index = 0;
	for (int t = arr + 1; ok && arr >= 0 && t < doc.doc.count && doc.doc.toks[t].start < doc.doc.toks[arr].end; t++) {
		if (doc.doc.toks[t].parent != arr)
			continue;
		char id[ND_ID_MAX], name[ND_PL_NAME];
		ok = doc.doc.toks[t].type == JSON_OBJECT && nd_pl_field(&doc.doc, t, "id", id, sizeof(id), true, false) && nd_pl_field(&doc.doc, t, "name", name, sizeof(name), true, false);
		if (!ok)
			break;
		sqlite3_reset(st);
		sqlite3_clear_bindings(st);
		sqlite3_bind_int(st, 1, index++);
		nd_pl_bind(st, 2, id);
		nd_pl_bind(st, 3, name);
		sqlite3_bind_int(st, 4, json_obj_bool(&doc.doc, t, "readonly", false));
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	nd_api_free(&doc);
	if (ok)
		ok = commit(s, c);
	if (!ok)
		nd_pl_sql(s, "ROLLBACK");
	if (leased)
		navidrome_cache_end();
	return ok;
}
static bool catalogue_window(nd_playlist_store_t *s, const nd_pl_request_t *q, nd_pl_result_t *out) {
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "SELECT count(*) FROM catalogue WHERE (?1='' OR name=?1)"))
		return false;
	nd_pl_bind(st, 1, q->name);
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	if (ok)
		out->total = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	if (!ok || !nd_pl_prepare(s, &st, "SELECT remote,name,readonly FROM catalogue WHERE (?2='' OR name=?2) ORDER BY name COLLATE NOCASE,remote LIMIT 16 OFFSET ?1"))
		return false;
	sqlite3_bind_int(st, 1, q->first);
	nd_pl_bind(st, 2, q->name);
	int rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		nd_playlist_t *p = &out->playlists[out->count++];
		snprintf(p->remote, sizeof(p->remote), "%s", sqlite3_column_text(st, 0));
		snprintf(p->name, sizeof(p->name), "%s", sqlite3_column_text(st, 1));
		p->readonly = sqlite3_column_int(st, 2);
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE;
}
typedef struct {
	char *text;
	size_t used, capacity;
} form_t;
static bool form_field(form_t *f, const char *key, const char *value) {
	char encoded[ND_PL_NAME * 3 + 1];
	if (strlen(value) >= ND_PL_NAME)
		return false;
	http_url_encode(value, encoded, sizeof(encoded));
	size_t need = f->used + strlen(key) + strlen(encoded) + 3;
	if (need > ND_PL_BYTES + 1)
		return false;
	if (need > f->capacity) {
		size_t cap = f->capacity ? f->capacity : 1024;
		while (cap < need)
			cap = cap > ND_PL_BYTES / 2 ? ND_PL_BYTES + 1 : cap * 2;
		char *grown = realloc(f->text, cap);
		if (!grown)
			return false;
		f->text = grown;
		f->capacity = cap;
	}
	int n = snprintf(f->text + f->used, f->capacity - f->used, "%s%s=%s", f->used ? "&" : "", key, encoded);
	f->used += (size_t)n;
	return true;
}
static void form_free(form_t *f) {
	if (f->text)
		nd_wipe(f->text, f->used);
	free(f->text);
	memset(f, 0, sizeof(*f));
}
static bool song_form(nd_playlist_store_t *s, const nd_playlist_t *p, form_t *f) {
	if (!form_field(f, *p->remote ? "playlistId" : "name", *p->remote ? p->remote : p->name))
		return false;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "SELECT track FROM entry WHERE p=? ORDER BY ordinal"))
		return false;
	sqlite3_bind_int64(st, 1, p->id);
	int rc;
	bool ok = true;
	while (ok && (rc = sqlite3_step(st)) == SQLITE_ROW)
		ok = form_field(f, "songId", (const char *)sqlite3_column_text(st, 0));
	sqlite3_finalize(st);
	return ok && rc == SQLITE_DONE;
}
static bool journal(nd_playlist_store_t *s, const nd_context_t *c, const nd_playlist_t *p, const char *hash, int pending, const char *remote, bool new_operation) {
	if (!nd_pl_begin(s, p->id, p->revision))
		return false;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "UPDATE playlist SET operation=operation+?5,intent_name=CASE WHEN ?5 THEN name ELSE intent_name END,pending=?1,desired=?2,remote=coalesce(nullif(?3,''),remote) WHERE id=?4"))
		return false;
	sqlite3_bind_int(st, 1, pending);
	nd_pl_bind(st, 2, hash);
	nd_pl_bind(st, 3, remote);
	sqlite3_bind_int64(st, 4, p->id);
	sqlite3_bind_int(st, 5, new_operation);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok && commit(s, c);
}
static bool verified(nd_playlist_store_t *s, const nd_context_t *c, const nd_playlist_t *p, const nd_playlist_t *remote, int64_t candidate, nd_pl_result_t *out) {
	char desired[ND_PL_HASH];
	if (!nd_pl_fingerprint(s, p->id, desired) || (strcmp(desired, remote->baseline) && (!p->pending || strcmp(p->desired, remote->baseline)))) {
		s->error = "nd_pl_verify_mismatch";
		return false;
	}
	if (!nd_pl_begin(s, p->id, p->revision))
		return false;
	bool ok = nd_pl_baseline(s, p->id, remote->remote, remote->baseline, remote->readonly, remote->changed) && statement_id(s, "DELETE FROM playlist WHERE id=?", candidate) && commit(s, c);
	return ok && nd_pl_get(s, p->id, &out->playlist);
}
static bool publish(nd_playlist_store_t *s, const nd_context_t *c, const nd_pl_request_t *q, nd_playlist_t *p, nd_pl_result_t *out) {
	bool creating = !*p->remote;
	if (creating && p->pending && q->action != NDPL_RECREATE) {
		s->error = "nd_pl_verify_needed";
		return false;
	}
	nd_playlist_t remote = {0};
	int64_t candidate = 0;
	char desired[ND_PL_HASH];
	if (!nd_pl_fingerprint(s, p->id, desired))
		return false;
	if (!creating) {
		if (!fetch(s, c, p->remote, &candidate, &remote))
			return false;
		if (remote.readonly) {
			s->error = "nd_pl_readonly";
			return false;
		}
		if (!strcmp(desired, remote.baseline))
			return verified(s, c, p, &remote, candidate, out);
		if (!q->confirmed || strcmp(q->checked, remote.baseline)) {
			out->confirm = true;
			out->conflict = strcmp(p->baseline, remote.baseline) != 0;
			out->playlist = *p;
			snprintf(out->checked, sizeof(out->checked), "%s", remote.baseline);
			return true;
		}
	}
	const char *error = capability(s, c);
	if (error) {
		s->error = error;
		return false;
	}
	form_t songs = {0}, rename = {0};
	bool ok = song_form(s, p, &songs);
	if (!creating && strcmp(remote.name, p->name))
		ok = ok && form_field(&rename, "playlistId", p->remote) && form_field(&rename, "name", p->name);
	if (!ok) {
		s->error = "nd_pl_too_large";
		form_free(&songs);
		form_free(&rename);
		return false;
	}
	// Intent is durable before either mutation; a lost response is never a clean failure.
	ok = journal(s, c, p, desired, creating ? 1 : 2, p->remote, true);
	if (!ok) {
		form_free(&songs);
		form_free(&rename);
		if (!s->error)
			s->error = "nd_pl_storage_error";
		return false;
	}
	nd_document_t doc = {0};
	if (ok) {
		error = api_store(s, c, "createPlaylist", NULL, songs.text, &doc);
		if (!error && creating) {
			int obj = json_get(&doc.doc, doc.response, "playlist");
			char id[ND_ID_MAX];
			if (!nd_pl_field(&doc.doc, obj, "id", id, sizeof(id), true, false))
				error = "nd_response_error";
			else if (!journal(s, c, p, desired, 2, id, false))
				error = "nd_pl_storage_error";
			else
				snprintf(p->remote, sizeof(p->remote), "%s", id);
		}
		if (!error)
			nd_api_free(&doc);
		else if (doc.body)
			nd_api_free(&doc);
		ok = !error;
	}
	form_free(&songs);
	if (ok && rename.text) {
		ok = journal(s, c, p, desired, 3, p->remote, false);
		error = ok ? api_store(s, c, "updatePlaylist", NULL, rename.text, &doc) : "nd_pl_storage_error";
		if (!error)
			nd_api_free(&doc);
		ok = !error;
	}
	form_free(&rename);
	if (ok)
		ok = journal(s, c, p, desired, 4, p->remote, false) && fetch(s, c, p->remote, &candidate, &remote) && verified(s, c, p, &remote, candidate, out);
	if (!ok) {
		s->error = error && (!strcmp(error, "nd_pl_readonly") || !strcmp(error, "nd_auth_error")) ? error : "nd_pl_verify_needed";
		return false;
	}
	return true;
}

void nd_pl_execute(const nd_pl_request_t *q, const nd_context_t *c, const nd_library_t *lib, nd_pl_result_t *out) {
	memset(out, 0, sizeof(*out));
	out->request = *q;
	out->context = *c;
	nd_wipe(out->context.password, sizeof(out->context.password));
	nd_playlist_store_t s = {0};
	char path[1200];
	bool read = readonly_action(q->action), network = network_action(q->action);
	if (!navidrome_context_current(c, network)) {
		out->error = "nd_pl_stale";
		return;
	}
	if (!nd_pl_path(lib, path)) {
		out->error = "nd_cache_storage";
		return;
	}
	if (!read && !lib->writable) {
		out->error = "nd_cache_readonly";
		return;
	}
	bool leased = navidrome_cache_begin(c->epoch, c->card_epoch);
	if (!leased) {
		out->error = "nd_pl_stale";
		return;
	}
	bool ok = true;
	if (read && access(path, F_OK) != 0 && q->action == NDPL_LIST) {
		if (!lib->writable)
			out->error = "nd_cache_readonly";
		navidrome_cache_end();
		return;
	}
	ok = nd_pl_open(&s, path, lib->scope, !read);
	s.leased = true;
	if (!ok)
		goto done;
	nd_playlist_t p = {0};
	bool needs_playlist = q->id > 0 && q->action != NDPL_LIST && q->action != NDPL_CATALOGUE && q->action != NDPL_CATALOGUE_WINDOW;
	if (needs_playlist) {
		ok = nd_pl_get(&s, q->id, &p);
		if (!ok)
			goto done;
	}
	if (needs_playlist && q->revision && p.revision != q->revision) {
		s.error = "nd_pl_stale";
		ok = false;
		goto done;
	}
	if (!read && q->action != NDPL_PULL_ACCEPT) {
		// Discard obsolete private candidates; stale confirmations fail safely.
		ok = nd_pl_sql(&s, "DELETE FROM playlist WHERE id<0");
		if (!ok)
			goto done;
	}
	switch (q->action) {
	case NDPL_LIST:
		ok = nd_pl_list(&s, q->first, out->playlists, &out->count, &out->total);
		break;
	case NDPL_ENTRIES:
		out->playlist = p;
		out->total = p.count;
		ok = nd_pl_entries(&s, q->id, q->first, q->session, out->entries, &out->count);
		break;
	case NDPL_CREATE: {
		int64_t id;
		ok = nd_pl_sql(&s, "BEGIN IMMEDIATE") && nd_pl_create(&s, q->name, &id) && commit(&s, c);
		if (ok)
			ok = nd_pl_get(&s, id, &out->playlist);
		break;
	}
	case NDPL_RENAME:
	case NDPL_DELETE:
		ok = nd_pl_edit(&s, p.id, p.revision, q->name, q->action == NDPL_DELETE) && commit(&s, c);
		if (ok && q->action == NDPL_RENAME)
			ok = nd_pl_get(&s, p.id, &out->playlist);
		break;
	case NDPL_COPY: {
		int64_t id;
		ok = nd_pl_copy(&s, p.id, q->name, &id) && commit(&s, c);
		if (ok)
			ok = nd_pl_get(&s, id, &out->playlist);
		break;
	}
	case NDPL_SELECT:
	case NDPL_SELECT_ONLY:
		if (q->generation && (!lib->ready || q->generation != lib->generation)) {
			s.error = "nd_pl_stale";
			ok = false;
			break;
		}
		ok = toggle(&s, q) && commit(&s, c);
		break;
	case NDPL_CLEAR:
		ok = nd_pl_sql(&s, "BEGIN IMMEDIATE") && statement_id(&s, "DELETE FROM selection WHERE session=?", q->session) && commit(&s, c);
		break;
	case NDPL_ADD:
		ok = add_selected(&s, q, c, lib, out);
		if (ok)
			ok = nd_pl_get(&s, p.id, &out->playlist);
		break;
	case NDPL_REMOVE: {
		ok = nd_pl_begin(&s, p.id, p.revision);
		sqlite3_stmt *st = NULL;
		if (ok)
			ok = nd_pl_prepare(&s, &st, q->occurrence ? "DELETE FROM entry WHERE p=? AND id=?" : "DELETE FROM entry WHERE p=? AND id IN (SELECT occurrence FROM selection WHERE session=?)");
		if (ok) {
			sqlite3_bind_int64(st, 1, p.id);
			sqlite3_bind_int64(st, 2, q->occurrence ? q->occurrence : q->session);
			ok = sqlite3_step(st) == SQLITE_DONE;
			out->added = sqlite3_changes(s.db);
		}
		sqlite3_finalize(st);
		if (ok)
			ok = nd_pl_finish(&s, p.id, out->added > 0) && statement_id(&s, "DELETE FROM selection WHERE session=?", q->session) && commit(&s, c);
		if (ok)
			ok = nd_pl_get(&s, p.id, &out->playlist);
		break;
	}
	case NDPL_CATALOGUE:
		ok = catalogue(&s, c);
		if (ok)
			ok = catalogue_window(&s, q, out);
		break;
	case NDPL_CATALOGUE_WINDOW:
		ok = catalogue_window(&s, q, out);
		break;
	case NDPL_IMPORT: {
		sqlite3_stmt *st = NULL;
		ok = nd_pl_prepare(&s, &st, "SELECT id FROM playlist WHERE remote=? AND id>0");
		int64_t id = 0;
		if (ok) {
			nd_pl_bind(st, 1, q->remote);
			int rc = sqlite3_step(st);
			if (rc == SQLITE_ROW)
				id = sqlite3_column_int64(st, 0);
			else
				ok = rc == SQLITE_DONE;
		}
		sqlite3_finalize(st);
		if (ok && id) {
			ok = nd_pl_get(&s, id, &out->playlist);
			break;
		}
		nd_playlist_t remote;
		int64_t candidate;
		if (ok)
			ok = fetch(&s, c, q->remote, &candidate, &remote) && install_candidate(&s, c, q, candidate, &remote, true, out);
		break;
	}
	case NDPL_PULL: {
		if (!*p.remote) {
			s.error = "nd_pl_missing";
			ok = false;
			break;
		}
		nd_playlist_t remote;
		int64_t candidate;
		ok = fetch(&s, c, p.remote, &candidate, &remote);
		if (ok && p.dirty) {
			out->confirm = true;
			out->playlist = p;
			out->candidate = candidate;
			snprintf(out->checked, sizeof(out->checked), "%s", remote.baseline);
		} else if (ok)
			ok = install_candidate(&s, c, q, candidate, &remote, false, out);
		break;
	}
	case NDPL_PULL_ACCEPT: {
		nd_playlist_t remote;
		ok = q->candidate < 0 && nd_pl_get(&s, q->candidate, &remote);
		char hash[ND_PL_HASH];
		if (ok)
			ok = nd_pl_fingerprint(&s, q->candidate, hash) && !strcmp(hash, q->checked);
		if (ok) {
			snprintf(remote.remote, sizeof(remote.remote), "%s", p.remote);
			snprintf(remote.baseline, sizeof(remote.baseline), "%s", hash);
			ok = install_candidate(&s, c, q, q->candidate, &remote, false, out);
		}
		break;
	}
	case NDPL_UPLOAD:
		if (p.remote[0] || p.pending) {
			s.error = p.pending ? "nd_pl_verify_needed" : "nd_pl_stale";
			ok = false;
			break;
		}
		ok = publish(&s, c, q, &p, out);
		break;
	case NDPL_PUBLISH:
	case NDPL_RECREATE:
		if (q->action == NDPL_RECREATE && (!q->confirmed || *p.remote || p.pending != 1)) {
			s.error = "nd_pl_stale";
			ok = false;
			break;
		}
		ok = publish(&s, c, q, &p, out);
		break;
	case NDPL_VERIFY:
	case NDPL_RECOVER: {
		const char *id = q->action == NDPL_RECOVER ? q->remote : p.remote;
		if (!*id || (q->action == NDPL_RECOVER && (!p.pending || !q->confirmed))) {
			s.error = "nd_pl_verify_needed";
			ok = false;
			break;
		}
		nd_playlist_t remote;
		int64_t candidate;
		ok = fetch(&s, c, id, &candidate, &remote) && verified(&s, c, &p, &remote, candidate, out);
		break;
	}
	}
	if (ok && s.db) {
		out->selected = selection_count(&s, q->session);
		if (out->selected < 0)
			ok = false;
	}
done:
	if (!ok)
		out->error = s.error ? s.error : "nd_pl_storage_error";
	if (s.db)
		nd_pl_close(&s);
	if (s.leased)
		navidrome_cache_end();
}

typedef struct {
	nd_pl_request_t q;
	nd_context_t c;
	nd_library_t lib;
	void (*done)(nd_pl_result_t *, void *);
	void *user;
	nd_pl_result_t *result;
} job_t;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static bool busy;
bool navidrome_playlists_busy(void) {
	pthread_mutex_lock(&jobs_lock);
	bool b = busy;
	pthread_mutex_unlock(&jobs_lock);
	return b;
}
static void delivered(void *data) {
	job_t *job = data;
	nd_pl_result_t *result = job->result;
	if (!navidrome_context_current(&result->context, false))
		result->error = "nd_pl_stale";
	if (job->done)
		job->done(result, job->user);
	else
		free(result);
	free(job);
}
static void *run_job(void *data) {
	job_t *job = data;
	nd_pl_execute(&job->q, &job->c, &job->lib, job->result);
	nd_wipe(&job->c, sizeof(job->c));
	navidrome_external_end();
	pthread_mutex_lock(&jobs_lock);
	busy = false;
	pthread_mutex_unlock(&jobs_lock);
	if (!gui_post(delivered, job)) {
		free(job->result);
		free(job);
	}
	return NULL;
}
bool navidrome_playlists_submit(const nd_pl_request_t *q, void (*done)(nd_pl_result_t *, void *), void *user) {
	pthread_mutex_lock(&jobs_lock);
	if (busy) {
		pthread_mutex_unlock(&jobs_lock);
		return false;
	}
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		pthread_mutex_unlock(&jobs_lock);
		return false;
	}
	job->result = calloc(1, sizeof(*job->result));
	bool network = network_action(q->action);
	navidrome_poll_network();
	bool ok = job->result && navidrome_context(&job->c, network);
	if (ok) {
		if (q->favorites)
			navidrome_favorites_snapshot(&job->lib);
		else
			navidrome_library_snapshot(&job->lib);
		ok = navidrome_external_begin(&job->c, network);
	}
	if (!ok) {
		nd_wipe(&job->c, sizeof(job->c));
		free(job->result);
		free(job);
		pthread_mutex_unlock(&jobs_lock);
		return false;
	}
	job->q = *q;
	job->done = done;
	job->user = user;
	busy = true;
	pthread_t thread;
	if (pthread_create(&thread, NULL, run_job, job)) {
		busy = false;
		navidrome_external_end();
		nd_wipe(&job->c, sizeof(job->c));
		free(job->result);
		free(job);
		pthread_mutex_unlock(&jobs_lock);
		return false;
	}
	pthread_detach(thread);
	pthread_mutex_unlock(&jobs_lock);
	return true;
}
