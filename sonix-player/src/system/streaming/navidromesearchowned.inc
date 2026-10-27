// Optional retained album readers for explicit actions. Ordinary Search callers
// do not allocate these wrappers. Retention shares existing bounded child data.
typedef struct child_owner {
	nd_owned_rows_t owned;
	child_t child;
	struct child_owner *next, *retired_next;
} child_owner_t;
static pthread_mutex_t child_owners_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t child_owners_ready = PTHREAD_COND_INITIALIZER;
static child_owner_t *child_owners, *child_retired;
static bool child_cleanup_started;
static bool child_owned_file(const char *path) {
	pthread_mutex_lock(&child_owners_lock);
	bool found = false;
	for (child_owner_t *p = child_owners; p; p = p->next)
		if (!strcmp(p->child.path, path)) {
			found = true;
			break;
		}
	pthread_mutex_unlock(&child_owners_lock);
	return found;
}
static void *child_cleanup(void *unused) {
	(void)unused;
	thread_be_background("navidrome child cleanup");
	for (;;) {
		pthread_mutex_lock(&child_owners_lock);
		while (!child_retired)
			pthread_cond_wait(&child_owners_ready, &child_owners_lock);
		child_owner_t *owner = child_retired;
		child_retired = owner->retired_next;
		pthread_mutex_unlock(&child_owners_lock);
		if (owner->child.path[0] && navidrome_playback_begin(&owner->owned.identity)) {
			struct stat st;
			if (!lstat(owner->child.path, &st) && S_ISREG(st.st_mode) && st.st_dev == owner->child.device && st.st_ino == owner->child.inode)
				unlink(owner->child.path);
			navidrome_cache_end();
		}
		pthread_mutex_lock(&child_owners_lock);
		child_owner_t **at = &child_owners;
		while (*at && *at != owner)
			at = &(*at)->next;
		if (*at)
			*at = owner->next;
		pthread_mutex_unlock(&child_owners_lock);
		free(owner->child.memory);
		free(owner);
	}
	return NULL;
}
static void child_owner_retire(nd_owned_rows_t *rows) {
	child_owner_t *owner = (child_owner_t *)rows;
	pthread_mutex_lock(&child_owners_lock);
	owner->retired_next = child_retired;
	child_retired = owner;
	pthread_cond_signal(&child_owners_ready);
	pthread_mutex_unlock(&child_owners_lock);
}
static int child_owner_read(const nd_owned_rows_t *rows, int first, nd_playrow_t *out, int limit) {
	const child_t *child = &((const child_owner_t *)rows)->child;
	if (first >= rows->count)
		return 0;
	int count = rows->count - first < limit ? rows->count - first : limit;
	nd_row_t values[ND_WINDOW];
	if (child->memory)
		memcpy(values, child->memory + first, (size_t)count * sizeof(*values));
	else {
		if (!navidrome_playback_begin(&rows->identity))
			return -1;
		sqlite3 *db = NULL;
		sqlite3_stmt *st = NULL;
		struct stat observed;
		bool ok = !lstat(child->path, &observed) && S_ISREG(observed.st_mode) && observed.st_dev == child->device && observed.st_ino == child->inode && sqlite3_open_v2(child->path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && sqlite3_exec(db, "PRAGMA cache_size=-64", NULL, NULL, NULL) == SQLITE_OK && sqlite3_prepare_v2(db, "SELECT data FROM row ORDER BY disc,track,key,id LIMIT ? OFFSET ?", -1, &st, NULL) == SQLITE_OK;
		int found = 0, rc = SQLITE_DONE;
		if (ok) {
			sqlite3_bind_int(st, 1, count);
			sqlite3_bind_int(st, 2, first);
			while ((rc = sqlite3_step(st)) == SQLITE_ROW && found < count) {
				if (sqlite3_column_bytes(st, 0) != sizeof(nd_row_t)) {
					ok = false;
					break;
				}
				memcpy(&values[found++], sqlite3_column_blob(st, 0), sizeof(nd_row_t));
			}
		}
		ok = ok && rc == SQLITE_DONE && found == count;
		sqlite3_finalize(st);
		if (db)
			sqlite3_close(db);
		navidrome_cache_end();
		if (!ok)
			return -1;
	}
	for (int i = 0; i < count; i++) {
		out[i] = (nd_playrow_t){.row = values[i]};
		memcpy(out[i].album_id, child->album.row.id, sizeof(out[i].album_id));
		memcpy(out[i].album, child->album.row.title, sizeof(out[i].album));
		memcpy(out[i].album_artist, child->album.album_artist, sizeof(out[i].album_artist));
		memcpy(out[i].artist_id, child->album.artist_id, sizeof(out[i].artist_id));
	}
	return count;
}
nd_owned_rows_t *nd_search_child_retain(nd_search_t *search, int level) {
	if (!search || level < 1 || level > 2 || !current(search))
		return NULL;
	child_t *child = &search->children[level - 1];
	if (child->source != ND_SEARCH_SERVER || child->kind != ND_TRACKS || child->count <= 0 || !child->album.row.id[0])
		return NULL;
	if (child->owner)
		return nd_owned_retain(child->owner);
	// Album-only passive membership already owns this immutable child.
	if (child->members)
		return NULL;
	pthread_mutex_lock(&child_owners_lock);
	if (!child_cleanup_started) {
		pthread_t thread;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		if (!pthread_create(&thread, &attr, child_cleanup, NULL)) {
			pthread_detach(thread);
			child_cleanup_started = true;
		}
		pthread_attr_destroy(&attr);
	}
	bool started = child_cleanup_started;
	pthread_mutex_unlock(&child_owners_lock);
	if (!started)
		return NULL;
	child_owner_t *owner = calloc(1, sizeof(*owner));
	if (!owner)
		return NULL;
	nd_context_t identity;
	if (!navidrome_context(&identity, false) || identity.profile_epoch != search->identity.profile_epoch || identity.card_epoch != child->library.card_epoch || !current(search)) {
		free(owner);
		nd_wipe(&identity, sizeof(identity));
		return NULL;
	}
	owner->child = *child;
	owner->owned = (nd_owned_rows_t){.refs = 1, .count = child->count, .identity = identity, .library = child->library, .read = child_owner_read, .destroy = child_owner_retire};
	nd_wipe(owner->owned.identity.password, sizeof(owner->owned.identity.password));
	nd_wipe(&identity, sizeof(identity));
	child->owner = &owner->owned;
	pthread_mutex_lock(&child_owners_lock);
	owner->next = child_owners;
	child_owners = owner;
	pthread_mutex_unlock(&child_owners_lock);
	return nd_owned_retain(child->owner);
}
