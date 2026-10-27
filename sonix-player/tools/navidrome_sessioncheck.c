// Production queue, service, durable stores, worker and restore coordinator.
// Decoder/output calls are counted fixtures; no real audio device is opened.
#define NAVIDROME_COORDINATOR_MAIN unused_old_coordinator
#define config_get_int base_config_get_int
#define __wrap_http_request base_http_request
#define __wrap_fsync base_fsync
#define thread_be_background base_thread_background
#define metadata_read base_metadata_read
#define device_state_represent_source base_represent_source
#define device_state_get base_device_state_get
#include "navidrome_coordinatorcheck.c"
#undef config_get_int
#undef __wrap_http_request
#undef __wrap_fsync
#undef thread_be_background
#undef metadata_read
#undef device_state_represent_source
#undef device_state_get
#include "src/system/playback/session.h"
#include "src/system/streaming/navidromelyrics.h"
#include "src/system/playback/sessionmusic.h"
#include "src/system/streaming/navidromeplayliststore.h"

static atomic_bool fail_publish_sync, fail_session_allocation;
void thread_be_background(const char *name) {
	char short_name[16];
	snprintf(short_name, sizeof(short_name), "%.15s", name);
	pthread_setname_np(pthread_self(), short_name);
}
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size) {
	char name[16] = {0};
	pthread_getname_np(pthread_self(), name, sizeof(name));
	if (count == 1 && size >= sizeof(session_row_t) && !strcmp(name, "session writer") && atomic_exchange(&fail_session_allocation, false))
		return NULL;
	return __real_calloc(count, size);
}
int __wrap_fsync(int fd) {
	struct stat st, parent;
	assert(!fstat(fd, &st));
	assert(!stat(".", &parent));
	if (S_ISDIR(st.st_mode) && st.st_dev == parent.st_dev && st.st_ino == parent.st_ino && atomic_exchange(&fail_publish_sync, false)) {
		errno = EIO;
		return -1;
	}
	return base_fsync(fd);
}
static bool remember_on = true;
bool qobuzcache_owns(const char *path) {
	(void)path;
	return false;
}
bool tidalcache_owns(const char *path) {
	(void)path;
	return false;
}
static int http_calls, restore_calls, report_calls, save_errors;
static char root_path[1024], music_db[1200], resume_ref[512];
static double resume_position;
static unsigned resume_queue;
void device_state_get(device_state_t *out) {
	base_device_state_get(out);
	if (resume_ref[0] && !strcmp(out->current_file, resume_ref) && resume_queue == playlist_identity()) {
		out->status = AUDIO_STATUS_PAUSED;
		out->progress_current_secs = resume_position;
	}
}
void device_state_represent_source(const char *ref, const song_metadata_t *meta, double duration) {
	bool retained = !strcmp(ref, resume_ref) && resume_queue == playlist_identity();
	base_represent_source(ref, meta, duration);
	if (retained) {
		playback.status = AUDIO_STATUS_PAUSED;
		playback.progress_current_secs = resume_position;
	}
}
long config_get_int(const char *section, const char *key, long fallback) {
	if (!strcmp(section, "player") && !strcmp(key, "remember_track"))
		return remember_on;
	return base_config_get_int(section, key, fallback);
}
bool __wrap_http_request(const char *url, const http_req_t *req, char **out, size_t *size, size_t limit, int timeout, int *status) {
	http_calls++;
	return base_http_request(url, req, out, size, limit, timeout, status);
}
bool power_screen_is_on(void) { return true; }
void toast_plain(const char *text) {
	(void)text;
	save_errors++;
}
void metadata_read(const char *path, song_metadata_t *m) {
	memset(m, 0, sizeof(*m));
	snprintf(m->title, sizeof(m->title), "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	m->has_tags = true;
}
library_index_t *library_index_clone(const library_index_t *ix) {
	assert(!ix);
	return NULL;
}
bool library_session_db_path(char *out, size_t size) { return snprintf(out, size, "%s", music_db) < (int)size; }
void player_restore_index(library_index_t *ix, int at, const char *const *extra, const int *slots, int count, const char *file, double position) {
	(void)ix;
	(void)at;
	(void)extra;
	(void)slots;
	(void)count;
	(void)file;
	(void)position;
	assert(!"unexpected Music query");
}
void player_restore_list(const char *const *paths, int count, int at, bool custom, const char *file, double position) {
	restore_calls++;
	playlist_load_paths(paths, count, at);
	playlist_set_custom(custom);
	snprintf(playback.current_file, sizeof(playback.current_file), "%s", file);
	playback.status = AUDIO_STATUS_PAUSED;
	playback.progress_current_secs = position;
	metadata_read(file, &playback.metadata);
}
void device_state_restore_representation(const char *ref, const song_metadata_t *m, double position, double duration) {
	device_state_represent_source(ref, m, duration);
	playback.status = AUDIO_STATUS_PAUSED;
	playback.progress_current_secs = position;
	snprintf(resume_ref, sizeof(resume_ref), "%s", ref);
	resume_position = position;
	resume_queue = playlist_identity();
}
double device_state_resume_position(const char *ref) { return !strcmp(ref, resume_ref) ? resume_position : 0; }
void device_state_play_prepared_at(const char *path, const song_metadata_t *m, bool paused, double duration, double position) {
	device_state_play_prepared(path, m, paused, duration);
	playback.progress_current_secs = position;
}
void nd_reports_start(const nd_context_t *context, const nd_library_t *lib, const char *id, const char *path, double duration) {
	(void)context;
	(void)lib;
	(void)id;
	(void)path;
	(void)duration;
	report_calls++;
}
static int legacy_calls;
static bool legacy_enabled;
static bool no_legacy(void) {
	legacy_calls++;
	if (!legacy_enabled)
		return false;
	char a[1200], b[1200];
	snprintf(a, sizeof(a), "%s/legacy-a.flac", root_path);
	snprintf(b, sizeof(b), "%s/legacy-b.flac", root_path);
	FILE *fa = fopen(a, "w"), *fb = fopen(b, "w");
	assert(fa && fb);
	fputs("fixture", fa);
	fputs("fixture", fb);
	fclose(fa);
	fclose(fb);
	const char *paths[] = {a, b, a};
	player_restore_list(paths, 3, 2, true, a, 29);
	return true;
}
static void pump(int count) {
	for (int i = 0; i < count; i++) {
		drain();
		lv_tick_inc(10);
		lv_timer_handler();
		navidromeplay_tick();
		usleep(1000);
	}
}
static session_header_t checkpoint(void) {
	playback_session_flush();
	pump(20);
	session_header_t h;
	assert(session_journal_read("playback-session.db", &h) == 1);
	return h;
}
static void setup(const char *root, bool seed_new) {
	snprintf(root_path, sizeof(root_path), "%s", root);
	assert(!chdir(root));
	navidrome_init();
	navidrome_set_root(root);
	if (seed_new) {
		assert(navidrome_save("http://fixture/base", "fixture user", server_password, NULL));
		wait_idle();
		nd_library_t lib;
		navidrome_library_snapshot(&lib);
		seed(&lib);
		navidrome_set_root(root);
	}
	nd_library_t lib;
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&lib);
		if (!lib.loading)
			break;
		usleep(1000);
	}
	navidrome_snapshot_t setup_state;
	navidrome_snapshot(&setup_state);
	if (setup_state.unlocked)
		assert(lib.scope[0]);
	char parent[1200];
	snprintf(parent, sizeof(parent), "%s/.local", root);
	assert(!mkdir(parent, 0700) || errno == EEXIST);
	snprintf(music_db, sizeof(music_db), "%s/.local/library.db", root);
	sqlite3 *db = NULL;
	assert(sqlite3_open(music_db, &db) == SQLITE_OK);
	sqlite3_close(db);
	playlist_set_card_root(root);
	lv_init();
	lv_display_t *display = lv_display_create(320, 480);
	lv_timer_pause(lv_display_get_refr_timer(display));
	lv_obj_t *origin = lv_obj_create(NULL);
	lv_screen_load(origin);
	lv_timer_pause(lv_display_get_refr_timer(display));
	navidromeplay_init();
	atomic_store(&allow_ready, true);
	http_calls = 0;
}
static void make_saved(void) {
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	char path[1200];
	nd_playlist_store_t store;
	assert(nd_pl_path(&lib, path) && nd_pl_open(&store, path, lib.scope, true));
	int64_t id;
	assert(nd_pl_create(&store, "Session fixture", &id) && nd_pl_begin(&store, id, 1));
	int added = 0, skipped = 0;
	for (int i = 0; i < 5000; i++) {
		nd_row_t row = {.duration = 300};
		snprintf(row.id, sizeof(row.id), "song-%05d", i % 2);
		snprintf(row.title, sizeof(row.title), "Saved occurrence %d", i);
		assert(nd_pl_append(&store, id, &row, false, &added, &skipped));
	}
	assert(nd_pl_finish(&store, id, true) && nd_pl_sql(&store, "COMMIT"));
	nd_playlist_t saved;
	assert(nd_pl_get(&store, id, &saved));
	nd_playlist_entry_t rows[ND_WINDOW];
	int count;
	assert(nd_pl_entries(&store, id, 0, 0, rows, &count));
	nd_pl_close(&store);
	assert(navidromeplay_playlist(&lib, id, saved.revision, 11, rows[11].occurrence, rows[11].track.id, lv_screen_active()));
	wait_started(1);
	assert(playlist_count() == 5000);
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
	assert(playlist_move(playlist_current_index(), 23));
	int gone = 1;
	assert(playlist_remove(&gone, 1));
	char file[1200];
	snprintf(file, sizeof(file), "%s/local.flac", root_path);
	FILE *f = fopen(file, "w");
	assert(f);
	fputs("fixture", f);
	fclose(f);
	queue_at_end = true;
	assert(playlist_enqueue(file));
	assert(playlist_count() == 5000);
	playback.status = AUDIO_STATUS_PAUSED;
	playback.progress_current_secs = 83;
}
int main(int argc, char **argv) {
	assert(argc == 3);
	alarm(90);
	const char *mode = argv[1];
	setup(argv[2], !strcmp(mode, "save"));
	if (!strcmp(mode, "legacy")) {
		legacy_enabled = true;
		playback_session_init(no_legacy);
		pump(300);
		session_header_t h = checkpoint();
		assert(h.owner == SESSION_MUSIC && h.ready && h.position == 29 && legacy_calls == 1 && playlist_current_index() == 2 && starts == 0 && http_calls == 0);
		puts("session integration: first-upgrade legacy Music is adopted once as an immutable generation");
		return 0;
	}
	if (!strcmp(mode, "pending") || !strcmp(mode, "corrupt")) {
		session_header_t h;
		assert(session_journal_read("playback-session.db", &h) == 1);
		if (!strcmp(mode, "pending")) {
			h.owner = SESSION_MUSIC;
			h.activation++;
			h.revision++;
			h.ready = 0;
			assert(session_journal_write("playback-session.db", &h));
		} else {
			sqlite3 *db = NULL;
			assert(sqlite3_open("playback-session.db", &db) == SQLITE_OK);
			assert(sqlite3_exec(db, "UPDATE header SET version=999", NULL, NULL, NULL) == SQLITE_OK);
			sqlite3_close(db);
		}
		legacy_enabled = true;
		playback_session_init(no_legacy);
		pump(300);
		assert(playback_session_blocked() && playlist_count() == 0 && legacy_calls == 0 && starts == 0 && http_calls == 0 && report_calls == 0);
		puts("session integration: interrupted/unsupported journal never falls back to older Music/Navidrome");
		return 0;
	}
	if (!strcmp(mode, "postfail"))
		atomic_store(&fixture_reject_posts, true);
	if (!strcmp(mode, "locked-music") || !strcmp(mode, "allocfail")) {
		playback_session_init(no_legacy);
		pump(100);
		if (!strcmp(mode, "locked-music"))
			assert(playback_session_blocked());
		char a[1200], b[1200];
		snprintf(a, sizeof(a), "%s/a.flac", root_path);
		snprintf(b, sizeof(b), "%s/b.flac", root_path);
		FILE *fa = fopen(a, "w"), *fb = fopen(b, "w");
		assert(fa && fb);
		fputs("fixture", fa);
		fputs("fixture", fb);
		fclose(fa);
		fclose(fb);
		const char *list[] = {a, b, a};
		playlist_load_paths(list, 3, 2);
		assert(strlen(a) < sizeof(playback.current_file));
		strcpy(playback.current_file, a);
		playback.status = AUDIO_STATUS_PLAYING;
		playback.progress_current_secs = 29;
		if (!strcmp(mode, "allocfail"))
			atomic_store(&fail_session_allocation, true);
		playback_session_note();
		session_header_t h = checkpoint();
		pump(100);
		if (!strcmp(mode, "allocfail")) {
			assert(h.owner == SESSION_MUSIC && !h.ready && save_errors > 0 && playlist_count() == 3);
			puts("session integration: worker result allocation failure fences the new owner, reports save failure and preserves live Music");
			return 0;
		}
		assert(h.owner == SESSION_MUSIC && h.ready && http_calls == 0);
		puts("session integration: Music supersedes a locked Navidrome restore without unlocking or network");
		return 0;
	}
	if (!strcmp(mode, "wrongcard")) {
		char marker[1300], old[33], changed[33];
		snprintf(marker, sizeof(marker), "%s/.local/navidrome/session-card", root_path);
		assert(session_card(root_path, false, old) && session_random_id(changed));
		FILE *f = fopen(marker, "w");
		assert(f);
		assert(fwrite(changed, 1, 32, f) == 32);
		fclose(f);
		playback_session_init(no_legacy);
		pump(300);
		assert(playback_session_blocked() && playlist_count() == 0 && starts == 0 && http_calls == 0 && report_calls == 0);
		assert(strcmp(playback.metadata.title, "Saved occurrence 11"));
		f = fopen(marker, "w");
		assert(f);
		assert(fwrite(old, 1, 32, f) == 32);
		fclose(f);
		puts("session integration: wrong card at same path remains paused/unavailable without exposing track metadata or contacting server");
		return 0;
	}
	if (!strcmp(mode, "unlock") || !strcmp(mode, "lateunlock")) {
		playback_session_init(no_legacy);
		pump(300);
		assert(playback_session_blocked() && playlist_count() == 0 && http_calls == 0);
		assert(navidrome_unlock("vault1"));
		if (!strcmp(mode, "lateunlock")) {
			char a[1200], b[1200];
			snprintf(a, sizeof(a), "%s/a.flac", root_path);
			snprintf(b, sizeof(b), "%s/b.flac", root_path);
			const char *paths[] = {a, b, a};
			player_restore_list(paths, 3, 2, true, a, 29);
			playback_session_note();
		}
		wait_idle();
		pump(500);
		assert(http_calls == 1 && starts == 0 && report_calls == 0);
		if (!strcmp(mode, "unlock")) {
			assert(!playback_session_blocked() && playlist_count() == 5000 && playlist_has_source());
			assert(playback.status == AUDIO_STATUS_PAUSED && playback.progress_current_secs == 83);
			assert(!strcmp(playback.metadata.title, "Saved occurrence 11"));
		} else {
			session_header_t h = checkpoint();
			assert(h.owner == SESSION_MUSIC && h.ready && playlist_count() == 3 && !playlist_has_source());
			assert(playback.progress_current_secs == 29);
		}
		puts("session integration: explicit Unlock checks once; restore adds no HTTP/audio/report and cannot replace newer Music");
		return 0;
	}
	if (!strcmp(mode, "missingcard")) {
		navidrome_set_root(NULL);
		playback_session_init(no_legacy);
		pump(300);
		assert(playback_session_blocked() && playlist_count() == 0 && starts == 0 && http_calls == 0);
		navidrome_set_root(root_path);
		for (int i = 0; playback_session_blocked(); i++) {
			assert(i < 10000);
			pump(1);
		}
		assert(playlist_count() == 5000 && playback.progress_current_secs == 83 && starts == 0 && http_calls == 0 && report_calls == 0);
		puts("session integration: matching card return completes only paused metadata without HTTP/audio/report");
		return 0;
	}
	if (!strcmp(mode, "reset")) {
		playback_session_init(no_legacy);
		playback_session_reset();
		assert(access("playback-session.db", F_OK) != 0);
		assert(access("playback-session.db.new", F_OK) != 0);
		assert(access("playback-session.db.lock", F_OK) != 0);
		pump(300);
		assert(access("playback-session.db", F_OK) != 0 && starts == 0 && http_calls == 0 && report_calls == 0);
		puts("session integration: reset joins session jobs before deletion; queued deliveries cannot recreate the journal");
		return 0;
	}
	if (!strcmp(mode, "profile")) {
		playback_session_init(no_legacy);
		assert(navidrome_save("http://other-fixture/base", "fixture user", server_password, NULL));
		wait_idle();
		pump(500);
		assert(playback_session_blocked() && playlist_count() == 0 && starts == 0 && report_calls == 0 && http_calls == 1);
		assert(strcmp(playback.metadata.title, "Saved occurrence 11"));
		puts("session integration: changed profile rejects pending saved metadata and adds no restore connection");
		return 0;
	}
	if (!strcmp(mode, "locked") || !strcmp(mode, "empty")) {
		playback_session_init(no_legacy);
		pump(300);
		if (!strcmp(mode, "locked")) {
			assert(playback_session_blocked() && playlist_count() == 0 && playback.status == AUDIO_STATUS_PAUSED && !strcmp(playback.metadata.title, "nd_unlock_required"));
		} else
			assert(!playback_session_blocked() && playlist_count() == 0 && legacy_calls == 0);
		assert(starts == 0 && http_calls == 0 && report_calls == 0);
		puts("session integration: locked/tombstoned startup preserves paused policy with no legacy fallback, audio or HTTP");
		return 0;
	}
	if (!strcmp(mode, "save")) {
		playback_session_init(no_legacy);
		pump(20);
		make_saved();
		playback_session_note();
		session_header_t h = checkpoint();
		assert(h.owner == SESSION_NAVIDROME && h.ready && h.count == 5000 && h.position == 83 && h.mode == PLAYBACK_MODE_SHUFFLE_REPEAT);
		assert(!save_errors);
		puts("session integration: saved edited mixed 5,000-occurrence shuffled session");
	} else {
		int before = http_calls;
		playback_session_init(no_legacy);
		pump(300);
		if (!strcmp(mode, "music")) {
			assert(restore_calls == 1 && playback.status == AUDIO_STATUS_PAUSED && playback.progress_current_secs == 29);
			assert(!playlist_has_source() && playlist_count() == 3 && playlist_current_index() == 2);
			assert(starts == 0 && http_calls == before && report_calls == 0);
			puts("session integration: restarted normal Music wins over prior Navidrome");
		} else {
			assert(!playback_session_blocked() && playlist_count() == 5000 && playlist_has_source());
			assert(playback.status == AUDIO_STATUS_PAUSED && playback.progress_current_secs == 83 && !strcmp(playback.metadata.title, "Saved occurrence 11"));
			assert(playlist_get_mode() == PLAYBACK_MODE_SHUFFLE_REPEAT && starts == 0 && http_calls == before && report_calls == 0);
			nd_playrow_t selected, lookup;
			nd_library_t selected_library, lookup_library;
			assert(navidromeplay_track(playback.current_file, &selected, &selected_library));
			assert(navidromeplay_lookup(playback.current_file, &lookup, &lookup_library));
			assert(!strcmp(selected.row.id, lookup.row.id) && !strcmp(selected_library.scope, lookup_library.scope));
			assert(!strcmp(selected.row.title, "Saved occurrence 11"));
			nd_lyric_target_t lyric;
			assert(nd_lyrics_target(playback.current_file, &lyric));
			assert(!strcmp(lyric.id, selected.row.id) && lyric.queue == playlist_identity());
			assert(!strcmp(lyric.library.scope, selected_library.scope) && !strcmp(lyric.reference, playback.current_file));
			assert(http_calls == before && report_calls == 0 && starts == 0);
			session_header_t h;
			assert(session_journal_read("playback-session.db", &h) == 1);
			char path[1300];
			assert(session_path(root_path, h.key, path, sizeof(path)));
			sqlite3 *db = NULL;
			uint32_t *expected = NULL;
			assert(session_payload_open(path, &h, &db, &expected));
			sqlite3_close(db);
			playlist_checkpoint_t *c = playlist_checkpoint_capture();
			assert(c);
			for (int i = 0; i < h.count; i++)
				assert(expected[i] == playlist_checkpoint_order(c)[i]);
			playlist_checkpoint_free(c);
			free(expected);
			if (!strcmp(mode, "uncertain")) {
				session_header_t previous = h;
				assert(playlist_move(0, 1));
				atomic_store(&fail_publish_sync, true);
				playback_session_note();
				h = checkpoint();
				pump(50);
				assert(h.ready && h.owner == SESSION_NAVIDROME && strcmp(previous.key, h.key) && save_errors > 0);
				char retained[1300];
				assert(session_path(root_path, h.key, retained, sizeof(retained)));
				assert(playlist_move(2, 3));
				atomic_store(&fail_sync, 1);
				playback_session_note();
				checkpoint();
				pump(50);
				session_header_t after;
				assert(session_journal_read("playback-session.db", &after) == 1 && !strcmp(after.key, h.key));
				sqlite3 *db = NULL;
				uint32_t *order = NULL;
				assert(session_payload_open(retained, &h, &db, &order));
				free(order);
				sqlite3_close(db);
				assert(save_errors >= 2 && playlist_count() == 5000 && playback.progress_current_secs == 83);
				puts("session integration: uncertain journal publication plus failed retry retains the actually referenced coherent payload");
			} else if (!strcmp(mode, "positionfail")) {
				session_header_t previous = h;
				playback.progress_current_secs = resume_position = 91;
				atomic_store(&fail_publish_sync, true);
				playback_session_note();
				h = checkpoint();
				pump(50);
				assert(h.ready && !strcmp(h.key, previous.key) && h.selected == previous.selected && h.position == 91 && save_errors > 0);
				puts("session integration: uncertain position publication stays bound to the same occurrence/payload");
			} else if (!strcmp(mode, "retirefail")) {
				atomic_store(&fail_publish_sync, true);
				playback_session_other_started();
				pump(50);
				h = checkpoint();
				pump(50);
				assert(h.owner == SESSION_NONE && !h.ready && save_errors > 0);
				puts("session integration: retirement durability failure cannot resurrect the prior session");
			} else if (!strcmp(mode, "failed")) {
				session_header_t previous = h;
				lv_obj_t *other = lv_obj_create(NULL);
				lv_screen_load(other);
				lv_timer_pause(lv_display_get_refr_timer(lv_display_get_default()));
				pump(50);
				nd_library_t lib;
				navidrome_library_snapshot(&lib);
				atomic_store(&fail_track, 16999);
				int before_fetch = atomic_load(&fetches);
				nd_query_t query = {.kind = ND_TRACKS};
				assert(navidromeplay_select(&lib, &query, "song-16999", other));
				for (int i = 0; atomic_load(&fetches) == before_fetch; i++) {
					assert(i < 10000);
					pump(1);
				}
				pump(50);
				h = checkpoint();
				assert(h.activation == previous.activation && h.revision == previous.revision && h.owner == SESSION_NAVIDROME && playlist_count() == 5000 && starts == 0 && playback.progress_current_secs == 83);
				puts("session integration: browsing and failed replacement preparation preserve the active checkpoint");
			} else if (!strcmp(mode, "other")) {
				playback_session_other_started();
				pump(50);
				h = checkpoint();
				assert(h.owner == SESSION_NONE && !h.ready);
				puts("session integration: non-restorable source immediately fences startup/old session ownership");
			} else if (!strcmp(mode, "postfail")) {
				assert(starts == 0 && http_calls == before && playback.progress_current_secs == 83);
				atomic_store(&fixture_reject_posts, false);
				puts("session integration: rejected GUI post recovers through the fixed result slot");
			} else if (!strcmp(mode, "encrypt")) {
				assert(navidrome_encrypt("vault1"));
				wait_idle();
				pump(100);
				h = checkpoint();
				assert(h.owner == SESSION_NAVIDROME && h.ready && h.position == 83 && starts == 0 && http_calls == before);
				puts("session integration: credential encryption revokes live source without discarding durable session");
			} else if (!strcmp(mode, "disable")) {
				remember_on = false;
				playback_session_settings();
				h = checkpoint();
				assert(h.owner == SESSION_NONE && !h.ready);
				puts("session integration: disabling Remember track retires both restore owners");
			} else if (!strcmp(mode, "forget")) {
				assert(navidrome_forget());
				wait_idle();
				pump(100);
				h = checkpoint();
				assert(h.owner == SESSION_NONE && !h.ready);
				puts("session integration: Forget retires matching session and rejects stale providers");
			} else if (!strcmp(mode, "clear")) {
				playlist_clear();
				playback.current_file[0] = 0;
				playback_session_note();
				h = checkpoint();
				assert(h.owner == SESSION_NONE && !h.ready);
				puts("session integration: deliberate queue clear durably retires the active owner");
			} else if (!strcmp(mode, "reenable")) {
				remember_on = false;
				playback_session_settings();
				h = checkpoint();
				assert(h.owner == SESSION_NONE);
				remember_on = true;
				playback_session_settings();
				h = checkpoint();
				assert(h.owner == SESSION_NAVIDROME && h.ready && h.count == 5000 && h.position == 83);
				puts("session integration: re-enabling Remember captures the current eligible queue");
			} else if (!strcmp(mode, "boundary")) {
				char next[512];
				assert(playlist_set_current(4999));
				c = playlist_checkpoint_capture();
				assert(c);
				assert(playlist_advance_auto(next, sizeof(next)) && playlist_current_index() == 0);
				playlist_checkpoint_t *fresh = playlist_checkpoint_capture();
				assert(fresh);
				bool seen[5000] = {0}, changed = false;
				for (int i = 0; i < 5000; i++) {
					uint32_t entry = playlist_checkpoint_order(fresh)[i];
					assert(entry < 5000 && !seen[entry]);
					seen[entry] = true;
					changed |= entry != playlist_checkpoint_order(c)[i];
				}
				assert(changed);
				playlist_checkpoint_free(c);
				playlist_checkpoint_free(fresh);
				puts("session integration: restored shuffle-repeat keeps its deal until the normal boundary, then redeals bijectively");
			} else if (!strcmp(mode, "canonical")) {
				char selected[512], after[512];
				assert(playlist_current_path(selected, sizeof(selected)));
				playlist_set_mode(PLAYBACK_MODE_NORMAL);
				assert(playlist_current_path(after, sizeof(after)) && !strcmp(after, selected));
				c = playlist_checkpoint_capture();
				assert(c);
				for (int i = 0; i < 5000; i++)
					assert(playlist_checkpoint_order(c)[i] == (uint32_t)i);
				playlist_checkpoint_free(c);
				puts("session integration: disabling restored shuffle retains exact occurrence and recovers canonical order");
			} else if (!strcmp(mode, "resumefail")) {
				nd_playrow_t selected;
				assert(navidromeplay_track(playback.current_file, &selected, NULL));
				atomic_store(&fail_track, atoi(selected.row.id + 5));
				int before_fetch = atomic_load(&fetches);
				assert(!navidromeplay_prepare(playback.current_file));
				for (int i = 0; atomic_load(&fetches) == before_fetch; i++) {
					assert(i < 10000);
					pump(1);
				}
				pump(300);
				assert(starts == 0 && report_calls == 0 && playback.progress_current_secs == 83 && playlist_current_index() == h.selected);
				device_state_t observed;
				device_state_get(&observed);
				assert(observed.status == AUDIO_STATUS_PAUSED);
				puts("session integration: failed explicit resume retains selected occurrence and paused position");
			} else if (!strcmp(mode, "resume") || !strcmp(mode, "permanent")) {
				permanent_download_paths = !strcmp(mode, "permanent");
				assert(!navidromeplay_prepare(playback.current_file));
				for (int i = 0; starts == 0; i++) {
					assert(i < 10000);
					pump(1);
				}
				assert(playback.progress_current_secs == 83 && report_calls == 1 && http_calls == before);
				if (permanent_download_paths)
					assert(strstr(playback.current_file, "/Music/Navidrome/"));
				puts("session integration: exact repeated occurrence/shuffle restored paused; explicit Play applies 83-second offset");
			} else if (!strcmp(mode, "switch")) {
				// A queued old-card restore must not replace a later deliberate Music session.
				navidrome_set_root(root_path);
				pump(2);
				char a[1200], b[1200];
				snprintf(a, sizeof(a), "%s/a.flac", root_path);
				snprintf(b, sizeof(b), "%s/b.flac", root_path);
				FILE *fa = fopen(a, "w"), *fb = fopen(b, "w");
				assert(fa && fb);
				fputs("fixture", fa);
				fputs("fixture", fb);
				fclose(fa);
				fclose(fb);
				const char *paths[] = {a, b, a};
				playlist_set_mode(PLAYBACK_MODE_NORMAL);
				playlist_load_paths(paths, 3, 2);
				assert(strlen(a) < sizeof(playback.current_file));
				strcpy(playback.current_file, a);
				playback.status = AUDIO_STATUS_PLAYING;
				playback.progress_current_secs = 29;
				playback_session_note();
				h = checkpoint();
				assert(h.owner == SESSION_MUSIC && h.ready);
				pump(100);
				assert(!playlist_has_source() && playlist_count() == 3 && playback.progress_current_secs == 29);
				puts("session integration: Music supersedes saved/pending Navidrome; late results rejected");
			}
		}
	}
	playback_session_flush();
	drain();
	return 0;
}
