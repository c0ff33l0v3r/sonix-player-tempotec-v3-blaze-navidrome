// Real permanent mapping/media validation, stores and local status worker.
// Reuse the existing synthetic service/decoder boundary; no network or device.
#define main unused_download_main
#include "navidrome_downloadcheck.c"
#undef main
#include "src/system/streaming/navidromeoffline.c"

bool navidrome_cache_begin(unsigned epoch, unsigned card_epoch) {
	if (epoch != library.epoch || card_epoch != context.card_epoch)
		return false;
	return navidrome_playback_begin(&context);
}
static atomic_bool local_preparing;
static atomic_int prepare_countdown;
bool navidromeplay_preparing(void) {
	if (atomic_load(&prepare_countdown) > 0 && atomic_fetch_sub(&prepare_countdown, 1) == 1)
		atomic_store(&local_preparing, true);
	return atomic_load(&local_preparing);
}
static nd_off_state_t observed[ND_OFF_ITEMS];
static bool delivered[ND_OFF_ITEMS];
static unsigned wanted_token;
static void offline_done(unsigned token, int row, nd_off_state_t state, void *user) {
	(void)user;
	assert(token == wanted_token && row >= 0 && row < ND_OFF_ITEMS);
	observed[row] = state;
	delivered[row] = true;
}
static void offline_wait(void) {
	double until = now() + 90;
	while (nd_offline_busy(wanted_token) && now() < until) {
		pump();
		nd_offline_poll();
		usleep(1000);
	}
	assert(!nd_offline_busy(wanted_token));
	pump();
	nd_offline_poll();
	assert(!leases);
}
static void offline_check(nd_off_request_t *q, nd_off_state_t state) {
	memset(delivered, 0, sizeof(delivered));
	wanted_token = nd_offline_submit(q, offline_done, NULL);
	assert(wanted_token);
	offline_wait();
	for (int i = 0; i < q->count; i++)
		assert(delivered[i] && observed[i] == state);
}
static atomic_bool fail_next_calloc;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size) {
	if (atomic_exchange(&fail_next_calloc, false))
		return NULL;
	return __real_calloc(count, size);
}
static void header_check(nd_off_request_t *q, nd_off_state_t header, nd_off_state_t row) {
	memset(delivered, 0, sizeof(delivered));
	wanted_token = nd_offline_submit(q, offline_done, NULL);
	assert(wanted_token);
	offline_wait();
	assert(delivered[ND_OFF_HEADER] && observed[ND_OFF_HEADER] == header);
	for (int i = 0; i < q->count; i++)
		assert(delivered[i] && observed[i] == row);
}
static void remote_header_checks(void) {
	nd_row_t *rows = calloc(2, sizeof(*rows));
	assert(rows);
	strcpy(rows[0].id, "A");
	strcpy(rows[1].id, "remote-only");
	atomic_store(&fail_next_calloc, true);
	assert(!nd_album_membership_create(&library, "album", rows, 2, "", 0, 0));
	nd_album_membership_t *members = nd_album_membership_create(&library, "album", rows, 2, "", 0, 0);
	assert(members);
	nd_off_request_t q = {.library = library, .kind = ND_OFF_TRACK, .count = 1};
	q.library.ready = false; // No Sync; represented remote membership differs from local A/B.
	strcpy(q.items[0].id, "A");
	strcpy(q.header.id, "album");
	q.header.members = members;
	atomic_store(&fail_next_calloc, true);
	assert(!nd_offline_submit(&q, offline_done, NULL));
	header_check(&q, ND_OFF_PARTIAL, ND_OFF_COMPLETE);
	q.library.writable = false;
	bool was_online = online;
	online = false;
	header_check(&q, ND_OFF_PARTIAL, ND_OFF_COMPLETE);
	online = was_online;
	q.library.writable = library.writable;
	for (int profile_change = 0; profile_change < 2; profile_change++) {
		memset(delivered, 0, sizeof(delivered));
		wanted_token = nd_offline_submit(&q, offline_done, NULL);
		assert(wanted_token);
		while (nd_offline_busy(wanted_token)) usleep(1000);
		if (profile_change) context.profile_epoch++; else context.card_epoch++;
		pump(); nd_offline_poll();
		assert(!delivered[ND_OFF_HEADER] && !delivered[0]);
		if (profile_change) context.profile_epoch--; else context.card_epoch--;
	}
	strcpy(q.header.id, "wrong-album");
	header_check(&q, ND_OFF_UNKNOWN, ND_OFF_COMPLETE);
	strcpy(q.header.id, "album");
	unsigned profile = context.profile_epoch++;
	assert(!nd_album_membership_matches(members, &library, "album"));
	context.profile_epoch = profile;
	header_check(&q, ND_OFF_PARTIAL, ND_OFF_COMPLETE);
	nd_album_membership_release(members);
	nd_album_membership_collect();

	// Real immutable child schema, with one fixed handle independent of its owner.
	char path[1200];
	snprintf(path, sizeof(path), "%s-members", library.path);
	sqlite3 *db;
	assert(sqlite3_open(path, &db) == SQLITE_OK);
	assert(sqlite3_exec(db, "CREATE TABLE row(id TEXT PRIMARY KEY,disc INT,track INT,key TEXT,data BLOB); WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<64) INSERT INTO row SELECT CASE x WHEN 0 THEN 'A' ELSE printf('remote/%d',x) END,x/16+1,x,'',NULL FROM n", NULL, NULL, NULL) == SQLITE_OK);
	assert(sqlite3_close(db) == SQLITE_OK);
	struct stat st;
	assert(!lstat(path, &st));
	members = nd_album_membership_create(&library, "album", NULL, 65, path, st.st_dev, st.st_ino);
	assert(members);
	q.header.members = nd_album_membership_retain(members);
	nd_album_membership_release(members); // original child owner leaves
	header_check(&q, ND_OFF_PARTIAL, ND_OFF_COMPLETE);
	nd_row_t window[ND_WINDOW]; int count, total;
	assert(nd_album_membership_read(q.header.members, 64, window, &count, &total) && count == 1 && total == 65);
	assert(!unlink(path));
	header_check(&q, ND_OFF_UNKNOWN, ND_OFF_COMPLETE);
	nd_album_membership_release(q.header.members);
	nd_album_membership_collect();
	assert(!leases && !cache_users);
	puts("PASS combined row/header, exact remote versus synced membership, retained 64/65 sources, allocation/read failures and profile retirement");
}
static nd_dl_job_t permanent(const char *card, const char *wav, const char *id, int number) {
	nd_dl_store_t store;
	assert(nd_dl_open(&store, &library, true));
	nd_playrow_t row = {0};
	snprintf(row.row.id, sizeof(row.row.id), "%s", id);
	int added = 0, skipped = 0;
	assert(nd_dl_insert(&store, 1, "Music/Navidrome", &row, 0, &added, &skipped) && added == 1);
	nd_dl_job_t job;
	assert(nd_dl_next(&store, &job));
	int dir = nd_dl_directory(card, "Music/Navidrome/Artist/Album", true);
	assert(dir >= 0);
	char leaf[40];
	snprintf(leaf, sizeof(leaf), "%d.wav", number);
	int out = openat(dir, leaf, O_CREAT | O_EXCL | O_WRONLY, 0600), in = open(wav, O_RDONLY);
	assert(out >= 0 && in >= 0);
	char buffer[4096];
	ssize_t n;
	while ((n = read(in, buffer, sizeof(buffer))) > 0)
		assert(write(out, buffer, n) == n);
	assert(!n && !close(in) && !close(out));
	struct stat st;
	assert(nd_dl_fingerprint(dir, leaf, job.digest, &st, NULL, NULL));
	close(dir);
	job.state = NDDL_COMPLETE;
	job.bytes = st.st_size;
	job.mtime = st.st_mtime;
	job.inode = st.st_ino;
	snprintf(job.path, sizeof(job.path), "Music/Navidrome/Artist/Album/%s", leaf);
	strcpy(job.extension, ".wav");
	assert(nd_dl_update(&store, &job));
	nd_dl_close(&store);
	return job;
}
int main(int argc, char **argv) {
	assert(argc == 3);
	int baseline_fds = descriptors();
	context.card_epoch = context.profile_epoch = context.epoch = 1;
	library.ready = library.writable = true;
	library.card_epoch = library.epoch = 1;
	library.generation = 1;
	memset(library.scope, 'a', 32);
	snprintf(library.path, sizeof(library.path), "%s/.local/navidrome/.cache/library.db", argv[1]);
	int dir = nd_dl_directory(argv[1], ".local/navidrome/.cache", true);
	assert(dir >= 0);
	close(dir);
	nd_store_t source;
	assert(nd_store_open(&source, library.path, library.scope, true, false));
	char *sql = sqlite3_mprintf("INSERT INTO snapshot(id,scope,complete,finished) VALUES(1,%Q,1,0); INSERT INTO active VALUES(%Q,1); INSERT INTO entity(gen,kind,id,title,artist,album,cover,duration,disc,track,sortkey) VALUES(1,0,'album','Album','Artist','','',0,1,0,'album'),(1,1,'A','A','Artist','album','',1,1,1,'a'),(1,1,'B','B','Artist','album','',1,2,1,'b');", library.scope, library.scope);
	assert(sqlite3_exec(source.db, sql, NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_free(sql);
	nd_store_close(&source);
	nd_off_request_t track = {.library = library, .kind = ND_OFF_TRACK, .count = 1};
	strcpy(track.items[0].id, "A");
	offline_check(&track, ND_OFF_NONE); // missing download store is not created
	nd_dl_store_t store;
	assert(nd_dl_open(&store, &library, true));
	assert(nd_dl_sql(&store, "INSERT INTO batch(id,destination,created) VALUES(1,'Music/Navidrome',0)"));
	nd_dl_close(&store);
	nd_dl_job_t a = permanent(argv[1], argv[2], "A", 1);
	offline_check(&track, ND_OFF_COMPLETE);
	nd_off_request_t album = {.library = library, .kind = ND_OFF_ALBUM, .count = 1};
	strcpy(album.items[0].id, "album");
	offline_check(&album, ND_OFF_PARTIAL); // B is on a different disc
	nd_off_request_t combined = track;
	strcpy(combined.header.id, "album");
	header_check(&combined, ND_OFF_PARTIAL, ND_OFF_COMPLETE);
	nd_dl_job_t b = permanent(argv[1], argv[2], "B", 2);
	offline_check(&album, ND_OFF_COMPLETE);
	header_check(&combined, ND_OFF_COMPLETE, ND_OFF_COMPLETE);
	remote_header_checks();
	puts("PASS real WAV downloads, missing store, complete and partial multidisc album");

	nd_playlist_store_t saved;
	char path[1200];
	assert(nd_pl_path(&library, path) && nd_pl_open(&saved, path, library.scope, true));
	int64_t playlist;
	assert(nd_pl_create(&saved, "Saved", &playlist) && nd_pl_begin(&saved, playlist, 1));
	int added = 0, skipped = 0;
	for (int i = 0; i < 5000; i++) {
		nd_row_t row = {0};
		strcpy(row.id, i % 2 ? "B" : "A");
		assert(nd_pl_append(&saved, playlist, &row, false, &added, &skipped));
	}
	assert(nd_pl_finish(&saved, playlist, true) && nd_pl_sql(&saved, "COMMIT"));
	nd_pl_close(&saved);
	nd_off_request_t pl = {.library = library, .kind = ND_OFF_PLAYLIST, .count = 1};
	pl.items[0].playlist = playlist;
	pl.items[0].revision = 2;
	library.ready = false;
	pl.library.ready = false;
	online = false;
	library.writable = false;
	assert(nd_dl_open(&store, &library, true) && nd_dl_clear_history(&store));
	nd_dl_close(&store);
	reject_posts = true; // GUI fallback must deliver all statuses
	offline_check(&pl, ND_OFF_COMPLETE);
	offline_check(&track, ND_OFF_COMPLETE);
	puts("PASS 5,000 repeated saved occurrences, no Sync/offline/read-only, hidden history, dropped GUI posts");
	reject_posts = false;
	assert(nd_pl_open(&saved, path, library.scope, true));
	assert(nd_pl_edit(&saved, playlist, 2, "New revision", false) && nd_pl_sql(&saved, "COMMIT"));
	nd_pl_close(&saved);
	offline_check(&pl, ND_OFF_UNKNOWN);
	pl.items[0].revision = 3;
	offline_check(&pl, ND_OFF_COMPLETE);
	strcpy(mode, "playlist_changed");
	offline_check(&pl, ND_OFF_UNKNOWN);
	mode[0] = 0;
	pl.items[0].revision = 4;
	offline_check(&pl, ND_OFF_COMPLETE);
	pl.items[0].playlist = 999;
	offline_check(&pl, ND_OFF_UNKNOWN);
	puts("PASS current saved revisions, stale/deleted saved identity rejection");

	char full[1200];
	snprintf(full, sizeof(full), "%s/%s", argv[1], a.path);
	assert(!unlink(full));
	offline_check(&track, ND_OFF_NONE);
	library.ready = true;
	offline_check(&album, ND_OFF_PARTIAL);
	assert(nd_dl_open(&store, &library, false));
	nd_dl_job_t unchanged;
	assert(nd_dl_find(&store, "A", &unchanged) && unchanged.state == NDDL_COMPLETE);
	nd_dl_close(&store); // passive checks never mark the job failed
	assert(!symlink(argv[2], full));
	offline_check(&track, ND_OFF_NONE);
	assert(!unlink(full));
	snprintf(full, sizeof(full), "%s/%s", argv[1], b.path);
	int fd = open(full, O_WRONLY);
	assert(fd >= 0 && write(fd, "FAIL", 4) == 4);
	close(fd);
	strcpy(track.items[0].id, "B");
	offline_check(&track, ND_OFF_NONE);
	strcpy(track.items[0].id, "cache-only");
	offline_check(&track, ND_OFF_NONE);
	assert(nd_dl_open(&store, &library, true));
	assert(nd_dl_sql(&store, "PRAGMA user_version=2"));
	nd_dl_close(&store);
	offline_check(&track, ND_OFF_UNKNOWN);
	char root_path[ND_DL_PATH], db_path[1200];
	assert(nd_dl_paths(&library, root_path, db_path));
	sqlite3 *raw;
	assert(sqlite3_open(db_path, &raw) == SQLITE_OK);
	assert(sqlite3_exec(raw, "PRAGMA user_version=1", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(raw);
	puts("PASS deleted/changed/symlink files, unsupported store without repair, disposable-only exclusion and no job-state mutation");

	assert(nd_store_open(&source, library.path, library.scope, true, false));
	assert(sqlite3_exec(source.db, "DELETE FROM entity WHERE kind=1; WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<17000) INSERT INTO entity(gen,kind,id,title,artist,album,cover,duration,disc,track,sortkey) SELECT 1,1,printf('large/%d',x),'Track','Artist','album','',1,x/100+1,x,printf('%05d',x) FROM n", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&source);
	double started_at = now();
	combined = (nd_off_request_t){.library = library, .kind = ND_OFF_TRACK, .count = ND_WINDOW};
	strcpy(combined.header.id, "album");
	for (int i = 0; i < ND_WINDOW; i++)
		snprintf(combined.items[i].id, ND_ID_MAX, "large/%d", i + 1);
	memset(delivered, 0, sizeof(delivered));
	wanted_token = nd_offline_submit(&combined, offline_done, NULL);
	assert(wanted_token);
	int replacements = 0;
	while (nd_offline_busy(wanted_token) && now() - started_at < 60) {
		usleep(30000);
		pump(); nd_offline_poll();
		if (delivered[ND_OFF_HEADER]) break;
		// Recycle row demand while complete-album progress survives matching replacement.
		nd_off_item_t first = combined.items[0];
		memmove(combined.items, combined.items + 1, (ND_WINDOW - 1) * sizeof(first));
		combined.items[ND_WINDOW - 1] = first;
		wanted_token = nd_offline_submit(&combined, offline_done, NULL);
		assert(wanted_token);
		replacements++;
	}
	offline_wait();
	assert(replacements > 10 && delivered[ND_OFF_HEADER] && observed[ND_OFF_HEADER] == ND_OFF_NONE);
	for (int i = 0; i < ND_WINDOW; i++) assert(delivered[i] && observed[i] == ND_OFF_NONE);
	printf("PASS %d live row-window replacements without starving the 17,000-track header\n", replacements);
	printf("PASS 17,000-track/171-disc album; fixed job %zu bytes, stack %d bytes; %.2f host seconds\n", sizeof(off_job_t), OFF_STACK, now() - started_at);

	// A foreground request arriving inside a mapping check must defer the
	// slice without publishing unknown or counting half the window twice.
	off_job_t *interrupted = calloc(1, sizeof(*interrupted));
	assert(interrupted);
	interrupted->request = album;
	interrupted->context = context;
	interrupted->token = off_serial;
	atomic_store(&prepare_countdown, 2);
	off_step(interrupted, 0);
	assert(atomic_load(&local_preparing) && !interrupted->rows[0].finished && !interrupted->rows[0].offset && !interrupted->rows[0].hits && !interrupted->rows[0].misses);
	atomic_store(&local_preparing, false);
	off_free(interrupted);
	// Local preparation is advisory too, independently of network ownership.
	atomic_store(&local_preparing, true);
	wanted_token = nd_offline_submit(&album, offline_done, NULL);
	assert(wanted_token);
	usleep(20000);
	assert(nd_offline_busy(wanted_token) && !leases);
	nd_offline_cancel(wanted_token);
	atomic_store(&local_preparing, false);
	// Advisory audio ownership defers work; cancellation cannot take its turn.
	streamturn_acquire();
	wanted_token = nd_offline_submit(&album, offline_done, NULL);
	assert(wanted_token);
	usleep(20000);
	assert(nd_offline_busy(wanted_token) && !leases);
	nd_offline_cancel(wanted_token);
	streamturn_release();
	wanted_token = nd_offline_submit(&track, offline_done, NULL);
	assert(wanted_token);
	offline_wait();
	puts("PASS audio-turn deferral, cancellation and replacement without ownership");
	// Completed-but-undelivered work must not expose a retired card/profile.
	memset(delivered, 0, sizeof(delivered));
	wanted_token = nd_offline_submit(&track, offline_done, NULL);
	assert(wanted_token);
	while (nd_offline_busy(wanted_token))
		usleep(1000);
	context.card_epoch++;
	pump();
	nd_offline_poll();
	assert(!delivered[0]);
	context.card_epoch--;
	wanted_token = nd_offline_submit(&track, offline_done, NULL);
	assert(wanted_token);
	while (nd_offline_busy(wanted_token))
		usleep(1000);
	alive = false;
	pump();
	nd_offline_poll();
	assert(!delivered[0]);
	alive = true;
	nd_offline_shutdown();
	pump();
	assert(!leases && !cache_users && descriptors() == baseline_fds);
	puts("PASS retired card/Lock delivery, shutdown and descriptor/lease release; no network used");
	return 0;
}
