#define NAVIDROME_PLAYLIST_UI
#define NAVIDROME_DOWNLOAD_UI
#define icon_download fixture_download_stub
#define icon_shuffle fixture_shuffle_stub
#define __wrap_http_request fixture_ping_request
#include "navidrome_browsecheck.c"
#undef __wrap_http_request
#undef icon_download
#undef icon_shuffle
extern const lv_image_dsc_t icon_download;
extern const lv_image_dsc_t icon_shuffle;
#include "src/gui/streaming/navidromeplaylists.c"
#ifdef NAVIDROME_EDGE_BASELINE
#define fixture_browse_slot(page, row) (row)
#else
#define fixture_browse_slot(page, row) ((page)->art_slots[(row)])
#endif
static nd_dl_request_t captured_download;
static unsigned download_submissions;
static nd_dl_captured_cb captured_download_cb;
static void *captured_download_user;
bool navidromedownloads_request(const nd_dl_request_t *request, nd_dl_captured_cb cb, void *user) {
	captured_download = *request;
	captured_download_cb = cb;
	captured_download_user = user;
	download_submissions++;
	return true;
}
static unsigned saved_taps, saved_leaves;
static int64_t saved_playlist, saved_revision, saved_occurrence;
static int saved_ordinal;
static char saved_track[ND_ID_MAX];
static unsigned saved_shortcuts, saved_additions;
static bool saved_shuffle;
static nd_playlist_capture_t saved_addition;
bool navidromeplay_playlist_shortcut(const nd_library_t *library, int64_t playlist,
		int64_t revision, int count, bool shuffle, void *origin) {
	assert(library->scope[0] && count > 0 && origin == lv_screen_active());
	saved_shortcuts++; saved_playlist = playlist; saved_revision = revision; saved_shuffle = shuffle;
	return true;
}
bool navidromeplay_playlist_enqueue(const nd_library_t *library, const nd_playlist_capture_t *request, void *origin) {
	assert(library->scope[0] && origin == lv_screen_active()); saved_addition = *request; saved_additions++; return true;
}
static device_state_t fixture_playing;
static nd_row_t fixture_track;
static nd_library_t fixture_play_library;
static char fixture_play_ref[512] = "ndq:fixture:0";
static bool fail_direct_step;
int __real_sqlite3_step(sqlite3_stmt *statement);
int __wrap_sqlite3_step(sqlite3_stmt *statement) {
	const char *sql = sqlite3_sql(statement);
	if (fail_direct_step && sql && !strncmp(sql, "INSERT INTO entry", 17)) { fail_direct_step = false; return SQLITE_FULL; }
	return __real_sqlite3_step(statement);
}
lv_obj_t *player_screen;
void device_state_get(device_state_t *out) { *out = fixture_playing; }
#ifndef NAVIDROME_OFFLINE_UI
bool nd_audio_source(const char *path) { return path && (!strncmp(path, "ndq:", 4) || !strcmp(path, "/fixture/permanent.flac")); }
#endif
bool navidromeplay_track(const char *path, nd_playrow_t *track, nd_library_t *library) {
	if (!nd_audio_source(path)) return false;
	if (track) { memset(track, 0, sizeof(*track)); track->row = fixture_track; }
	if (library) *library = fixture_play_library;
	return true;
}
bool playlist_current_path(char *path, size_t cap) { snprintf(path, cap, "%s", fixture_play_ref); return true; }
void player_sheet_close(bool animated) { (void)animated; }
void switcher_set_player_return(lv_obj_t *screen) { (void)screen; }
static int menu_index(const char *label) {
	for (int i = 0; i < fixture_menu_count; i++) if (!strcmp(fixture_menu[i].label, label)) return i;
	return -1;
}
static void menu_activate(const char *label) {
	int i = menu_index(label); assert(i >= 0); fixture_menu[i].action(fixture_menu[i].user);
}
bool navidromeplay_playlist(const nd_library_t *library, int64_t playlist, int64_t revision,
		int ordinal, int64_t occurrence, const char *track, void *origin) {
	assert(*library->scope && *library->path && origin == lv_screen_active());
	saved_taps++;
	saved_playlist = playlist; saved_revision = revision; saved_occurrence = occurrence; saved_ordinal = ordinal;
	snprintf(saved_track, sizeof(saved_track), "%s", track);
	return true;
}
void navidromeplay_playlist_leave(void *origin) { if (origin == pl_pages[1].screen) saved_leaves++; }
lv_font_t font_ui_16, font_ui_24;
const lv_image_dsc_t icon_list_music = FIXTURE_ICON;
const lv_image_dsc_t icon_plus = FIXTURE_ICON, icon_import = FIXTURE_ICON, icon_refresh = FIXTURE_ICON, icon_trash = FIXTURE_ICON;
static struct {
	char id[ND_ID_MAX];
	int size;
	bool pending;
} fixture_art[ND_ART_SLOT_COUNT];
static bool fixture_art_ready;
static unsigned fixture_art_requests;
static unsigned fixture_art_releases;
void navidromeart_release(int slot) {
	assert(slot >= 0 && slot < ND_ART_SLOT_COUNT);
	fixture_art_releases++;
	memset(&fixture_art[slot], 0, sizeof(fixture_art[slot]));
}
void navidromeart_request(int slot, const nd_library_t *library, const char *id, int size) {
	assert(slot >= 0 && slot < ND_ART_SLOT_COUNT && *library->scope && *library->path);
	assert(slot >= ND_ART_ALBUM_SLOT || size == ui_px(ND_ART_THUMB_SIZE));
	snprintf(fixture_art[slot].id, sizeof(fixture_art[slot].id), "%s", id);
	fixture_art[slot].size = size;
	fixture_art[slot].pending = true;
	fixture_art_requests++;
}
bool navidromeart_take(int slot, cover_image_t *out, bool *finished) {
	*finished = fixture_art_ready || !fixture_art[slot].pending;
	if (!fixture_art_ready || !fixture_art[slot].pending) return false;
	int size = fixture_art[slot].size;
	out->pixels = calloc((size_t)size * size, 4);
	assert(out->pixels);
	for (int i = 0; i < size * size; i++) {
		out->pixels[i * 4 + 2] = 255;
		out->pixels[i * 4 + 3] = 255;
	}
	out->dsc = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = size, .h = size, .stride = size * 4}, .data = out->pixels, .data_size = (uint32_t)(size * size * 4)};
	fixture_art[slot].pending = false;
	return true;
}
static void assert_cover_rendered(lv_obj_t *image) {
	lv_obj_update_layout(image);
	lv_draw_buf_t *snapshot = lv_snapshot_take(image, LV_COLOR_FORMAT_ARGB8888);
	assert(snapshot);
	int red = 0;
	for (unsigned y = 0; y < snapshot->header.h; y++)
		for (unsigned x = 0; x < snapshot->header.w; x++) {
			const unsigned char *px = snapshot->data + y * snapshot->header.stride + x * 4;
			red += px[2] > 200 && px[1] < 30 && px[0] < 30 && px[3] > 200;
		}
	int expected = ui_px(ND_ART_THUMB_SIZE) * ui_px(ND_ART_THUMB_SIZE);
	fprintf(stderr, "cover snapshot: drawn_pixels=%d expected=%d scale=%d\n", red, expected, (int)lv_image_get_scale_x(image));
	lv_draw_buf_destroy(snapshot);
	assert(red >= expected * 95 / 100);
	assert(lv_image_get_scale_x(image) == LV_SCALE_NONE);
}
static atomic_bool fixture_delay_import, fixture_import_entered, fixture_finish_import;
static char fixture_upload_name[128];
static int fixture_upload_count, fixture_upload_creates;
static bool fixture_upload_lost, fixture_upload_bad_verify;
bool __wrap_http_request(const char *url, const http_req_t *req, char **out, size_t *size, size_t limit, int timeout, int *status) {
	if (fixture_upload_name[0] && (strstr(url, "/getOpenSubsonicExtensions.") ||
		strstr(url, "/createPlaylist.") || strstr(url, "id=ui-created-"))) {
		char response[4096];
		assert(req->sensitive_url && limit == ND_PL_BYTES + 1);
		atomic_fetch_add(&calls, 1);
		if (strstr(url, "/getOpenSubsonicExtensions."))
			snprintf(response, sizeof(response), "{\"subsonic-response\":{\"status\":\"ok\",\"openSubsonicExtensions\":[{\"name\":\"formPost\",\"versions\":[1]}]}}");
		else if (strstr(url, "/createPlaylist.")) {
			assert(req->method && !strcmp(req->method, "POST") && req->body && strstr(req->body, "name="));
			int songs = 0; for (const char *p = req->body; (p = strstr(p, "songId=")); p += 7) songs++;
			assert(songs == fixture_upload_count);
			fixture_upload_creates++;
			if (fixture_upload_lost) snprintf(response, sizeof(response), "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{}}}");
			else snprintf(response, sizeof(response), "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"ui-created-%d\"}}}", fixture_upload_creates);
		} else {
			int used = snprintf(response, sizeof(response), "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"ui-created-%d\",\"name\":\"%s\",\"songCount\":%d,\"entry\":[", fixture_upload_creates, fixture_upload_bad_verify ? "Changed remotely" : fixture_upload_name, fixture_upload_count);
			for (int i = 0; i < fixture_upload_count; i++) used += snprintf(response + used, sizeof(response) - (size_t)used, "%s{\"id\":\"song-%05d\",\"title\":\"Track\"}", i ? "," : "", i % 2);
			snprintf(response + used, sizeof(response) - (size_t)used, "]}}}");
		}
		*out = strdup(response); *size = strlen(response); *status = 200; return true;
	}
	if (!strstr(url, "/getPlaylist"))
		return fixture_ping_request(url, req, out, size, limit, timeout, status);
	assert(limit == ND_PL_BYTES + 1 && req->sensitive_url && !req->method);
	atomic_fetch_add(&calls, 1);
	const char *body;
	if (strstr(url, "/getPlaylists."))
		body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlists\":{\"playlist\":[{\"id\":\"fixture-empty\",\"name\":\"Empty remote\"},{\"id\":\"fixture-art\",\"name\":\"Artwork remote\"},{\"id\":\"fixture-delayed\",\"name\":\"Delayed remote\"}]}}}";
	else if (strstr(url, "id=fixture-art"))
		body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"fixture-art\",\"name\":\"Artwork remote\",\"songCount\":2,\"entry\":[{\"id\":\"remote-art-track\",\"title\":\"With art\",\"coverArt\":\"remote-cover\"},{\"id\":\"remote-no-art\",\"title\":\"No art\"}]}}}";
	else if (strstr(url, "id=fixture-delayed")) {
		atomic_store(&fixture_import_entered, true);
		while (atomic_load(&fixture_delay_import) && !atomic_load(&fixture_finish_import)) usleep(1000);
		body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"fixture-delayed\",\"name\":\"Delayed remote\",\"songCount\":0,\"entry\":[]}}}";
	} else {
		assert(strstr(url, "id=fixture-empty"));
		body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"fixture-empty\",\"name\":\"Empty remote\",\"songCount\":0,\"entry\":[]}}}";
	}
	*out = strdup(body);
	*size = strlen(body);
	*status = 200;
	return true;
}
void confirm_close(void) {}
static void (*fixture_confirm)(void *);
static void *fixture_confirm_user;
bool switcher_in_history(lv_obj_t *screen) {
	(void)screen;
	return true;
}
keyboard_t *keyboard_create(lv_obj_t *parent, int width, int height, lv_obj_t *field, const lv_image_dsc_t *icon, const char *text, lv_event_cb_t cb, void *user) {
	(void)parent;
	(void)width;
	(void)height;
	(void)field;
	(void)icon;
	(void)text;
	(void)cb;
	(void)user;
	return NULL;
}
void keyboard_style_caret(lv_obj_t *field) { (void)field; }
void keyboard_reset(keyboard_t *kb) { (void)kb; }
void confirm_show(const char *title, const char *message, const char *ok, void (*cb)(void *), void *user) {
	(void)title;
	(void)message;
	(void)ok;
	fixture_confirm = cb;
	fixture_confirm_user = user;
}
static lv_indev_data_t fixture_touch;
static void fixture_touch_read(lv_indev_t *input, lv_indev_data_t *data) {
	(void)input;
	*data = fixture_touch;
}
static void pl_wait(int page) {
	for (int i = 0; i < 10000; i++) {
		drain();
		lv_obj_update_layout(pl_pages[page].screen);
		if (!navidrome_playlists_busy() && !pl_pages[page].loading)
			return;
		usleep(1000);
	}
	assert(!"playlist UI timeout");
}
static void selection_wait(void) {
	for (int i = 0; i < 10000; i++) {
		drain();
		if (!navidrome_playlists_busy() && !pages[0].select_busy && pages[0].selecting) {
			window_wait(0);
			return;
		}
		usleep(1000);
	}
	assert(!"selection timeout");
}
static void library_wait(nd_library_t *lib) {
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(lib);
		if (!lib->loading)
			return;
		usleep(1000);
	}
	assert(!"library timeout");
}
static void pick_wait(int level) {
	for (int i = 0; i < 10000; i++) {
		drain();
		if (!navidrome_playlists_busy() && !pages[level].select_busy) {
			window_wait(level);
			return;
		}
		usleep(1000);
	}
	assert(!"picker selection timeout");
}
static int remote_index(const char *id) {
	for (int i = 0; i < pl_pages[3].count; i++)
		if (!strcmp(pl_pages[3].playlists[i].remote, id)) return i;
	assert(!"remote catalogue ID missing");
	return -1;
}
static void playlist_actions_ui(nd_library_t *lib) {
	char path[1200]; nd_playlist_store_t store;
	assert(nd_pl_path(lib, path) && nd_pl_open(&store, path, lib->scope, true));
	int64_t destination;
	assert(nd_pl_create(&store, "! Action destination", &destination));
	nd_playlist_t target;
	assert(nd_pl_get(&store, destination, &target)); nd_pl_close(&store);
	fixture_play_library = *lib;
	player_screen = lv_obj_create(NULL); switch_screen(player_screen);
	strcpy(fixture_playing.current_file, "/fixture/permanent.flac");
	strcpy(fixture_track.id, "unsynced-A"); strcpy(fixture_track.title, "Captured A");
	strcpy(fixture_track.artist, "Saved fallback"); fixture_track.duration = 123;
	assert(!lib->ready && navidromeplaylists_bind_current(fixture_playing.current_file));
	strcpy(fixture_track.id, "unsynced-B");
	assert(!navidromeplaylists_add_current()); // Reject the stale menu before picker entry.
	strcpy(fixture_track.id, "unsynced-A");
	int requests = atomic_load(&calls);
	assert(navidromeplaylists_bind_current(fixture_playing.current_file) && navidromeplaylists_add_current());
	pl_wait(2);
	strcpy(fixture_track.id, "unsynced-B"); strcpy(fixture_play_ref, "ndq:fixture:1");
	int index = -1;
	for (int i = 0; i < pl_pages[2].count; i++) if (pl_pages[2].playlists[i].id == destination) index = i;
	assert(index >= 0);
	lv_obj_send_event(pl_pages[2].rows[index], LV_EVENT_CLICKED, NULL); pl_wait(2);
	assert(!pl_direct_active && lv_screen_active() == player_screen);
	assert(nd_pl_open(&store, path, lib->scope, false) && nd_pl_get(&store, destination, &target));
	nd_playlist_entry_t entries[ND_WINDOW]; int n;
	assert(target.count == 1 && nd_pl_entries(&store, destination, 0, 0, entries, &n) && n == 1);
	assert(!strcmp(entries[0].track.id, "unsynced-A") && !strcmp(entries[0].track.title, "Captured A"));
	nd_pl_close(&store);
	// Same ID is a no-op despite changed metadata; direct writes need no source DB.
	nd_context_t context; assert(navidrome_context(&context, false));
	nd_pl_request_t q = {.action = NDPL_ADD, .id = destination, .revision = target.revision, .direct = true};
	q.source = (nd_pl_track_t){.row = entries[0].track, .profile = context.profile_epoch, .card = context.card_epoch};
	strcpy(q.source.scope, lib->scope);
	strcpy(q.source.row.title, "Changed metadata");
	nd_pl_result_t *result = calloc(1, sizeof(*result)); assert(result);
	nd_pl_execute(&q, &context, lib, result);
	assert(!result->error && result->added == 0 && result->skipped == 1 && result->playlist.revision == target.revision);
	q.source.profile++; nd_pl_execute(&q, &context, lib, result); assert(result->error); q.source.profile--;
	q.revision--; nd_pl_execute(&q, &context, lib, result); assert(result->error); q.revision++;
	nd_library_t readonly = *lib; readonly.writable = false;
	nd_pl_execute(&q, &context, &readonly, result); assert(result->error);
	strcpy(q.source.row.id, "unsynced-C"); fail_direct_step = true;
	nd_pl_execute(&q, &context, lib, result); assert(result->error && !fail_direct_step);
	assert(nd_pl_open(&store, path, lib->scope, false));
	nd_playlist_t unchanged; assert(nd_pl_get(&store, destination, &unchanged));
	assert(unchanged.count == 1 && unchanged.revision == target.revision); nd_pl_close(&store);
	free(result); nd_wipe(&context, sizeof(context));
	// New destination creation carries the same direct target; cancellation does not add.
	assert(navidromeplaylists_bind_current(fixture_playing.current_file) && navidromeplaylists_add_current());
	pl_wait(2); lv_obj_send_event(pl_pages[2].new_row, LV_EVENT_CLICKED, NULL);
	assert(pl_back());
	lv_obj_send_event(pl_pages[2].new_row, LV_EVENT_CLICKED, NULL);
	lv_textarea_set_text(pl_name_field, "Action new destination"); pl_name_accept(NULL); pl_wait(2);
	assert(lv_screen_active() == player_screen && !pl_direct_active);
	assert(atomic_load(&calls) == requests);
	// Real controls and menus bind whole/entry identity independently of the visible band.
	pl_open_detail(&target); pl_wait(1);
	lv_obj_update_layout(pl_pages[1].screen);
	lv_area_t actions, list;
	lv_obj_get_coords(pl_pages[1].play_actions, &actions); lv_obj_get_coords(pl_pages[1].list, &list);
	assert(actions.y2 < list.y1 && actions.x1 >= 0 && actions.x2 < ui_panel_width);
	lv_obj_t *shuffle_glyph = lv_obj_get_child(pl_pages[1].play_buttons[1], 0);
	assert(icon_shuffle.header.w > 1 && lv_image_get_src(shuffle_glyph) == &icon_shuffle);
	lv_area_t glyph, button; lv_obj_get_coords(shuffle_glyph, &glyph); lv_obj_get_coords(pl_pages[1].play_buttons[1], &button);
	assert(glyph.x1 >= button.x1 && glyph.x2 <= button.x2 && glyph.y1 >= button.y1 && glyph.y2 <= button.y2);
	unsigned shortcuts = saved_shortcuts;
	lv_obj_send_event(pl_pages[1].play_buttons[0], LV_EVENT_CLICKED, NULL);
	assert(saved_shortcuts == shortcuts + 1 && !saved_shuffle && saved_playlist == destination);
	lv_obj_send_event(pl_pages[1].play_buttons[1], LV_EVENT_CLICKED, NULL);
	assert(saved_shortcuts == shortcuts + 2 && saved_shuffle);
	lv_obj_send_event(pl_pages[1].menus[0], LV_EVENT_CLICKED, NULL);
	unsigned queued = saved_additions;
	menu_activate("add_to_queue"); assert(saved_additions == queued + 1 && saved_addition.single && !saved_addition.whole);
	assert(saved_addition.occurrence == pl_pages[1].entries[0].occurrence && !strcmp(saved_addition.track, "unsynced-A"));
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	menu_activate("add_to_queue"); assert(saved_additions == queued + 2 && saved_addition.whole);
	int queue_item = menu_index("add_to_queue"); popover_item_t stale = fixture_menu[queue_item];
	pl_pages[1].serial++; stale.action(stale.user); assert(saved_additions == queued + 2);
	unsigned starts_before_gesture = saved_shortcuts;
	dragging = true; lv_obj_send_event(pl_pages[1].play_buttons[0], LV_EVENT_CLICKED, NULL); dragging = false;
	assert(saved_shortcuts == starts_before_gesture);
	pl_pages[1].selecting = true; pl_buttons(&pl_pages[1]); assert(lv_obj_is_hidden(pl_pages[1].play_actions));
	pl_pages[1].selecting = false; pl_buttons(&pl_pages[1]); assert(!lv_obj_is_hidden(pl_pages[1].play_actions));
	assert(atomic_load(&calls) == requests);
	puts("playlist actions UI: real no-Sync picker/direct dedupe/New flow, retained target after skip, stale guards, Play/Shuffle geometry and whole/entry queue menus: passed");
}
static void playlist_upload_ui(nd_library_t *lib) {
	atomic_store(&mode, 0); assert(navidrome_reconnect()); wait_idle();
	navidrome_library_snapshot(lib); assert(!lib->ready);
	for (int phase = 0; phase < 4; phase++) {
		char path[1200]; nd_playlist_store_t store; int64_t id;
		snprintf(fixture_upload_name, sizeof(fixture_upload_name), "Upload fixture %d", phase);
		fixture_upload_count = phase == 1 ? 3 : 0;
		fixture_upload_lost = phase == 2; fixture_upload_bad_verify = phase == 3;
		assert(nd_pl_path(lib, path) && nd_pl_open(&store, path, lib->scope, true));
		assert(nd_pl_create(&store, fixture_upload_name, &id));
		if (fixture_upload_count) {
			assert(nd_pl_begin(&store, id, 1)); int added = 0, skipped = 0;
			for (int i = 0; i < 3; i++) { nd_row_t row = {.title = "Track"}; snprintf(row.id, sizeof(row.id), "song-%05d", i % 2); assert(nd_pl_append(&store, id, &row, false, &added, &skipped)); }
			assert(nd_pl_finish(&store, id, true) && nd_pl_sql(&store, "COMMIT"));
		}
		nd_playlist_t target; assert(nd_pl_get(&store, id, &target)); nd_pl_close(&store);
		int creates = fixture_upload_creates;
		if (phase == 1 || phase == 3) {
			pl_open_detail(&target); pl_wait(1);
			lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
		} else {
			navidromeplaylists_open(); pl_wait(0); int index = -1;
			for (int i = 0; i < pl_pages[0].count; i++) if (pl_pages[0].playlists[i].id == id) index = i;
			assert(index >= 0); lv_obj_send_event(pl_pages[0].menus[index], LV_EVENT_CLICKED, NULL);
		}
		int page = phase == 1 || phase == 3 ? 1 : 0;
		assert(menu_index("nd_pl_upload") >= 0);
		int upload = menu_index("nd_pl_upload"); popover_item_t old_upload = fixture_menu[upload];
		menu_activate("nd_pl_upload"); pl_wait(page);
		assert(fixture_upload_creates == creates + 1 && pl_active(page));
		assert(nd_pl_open(&store, path, lib->scope, false) && nd_pl_get(&store, id, &target)); nd_pl_close(&store);
		if (phase < 2) assert(target.remote[0] && !target.pending && target.id == id && !target.dirty);
		else {
			assert(target.pending && (phase == 2 ? !target.remote[0] : target.remote[0] != 0));
			nd_context_t context; assert(navidrome_context(&context, false));
			nd_pl_request_t q = {.action = NDPL_UPLOAD, .id = id, .revision = target.revision};
			nd_pl_result_t *result = calloc(1, sizeof(*result)); assert(result);
			int calls_before = atomic_load(&calls); nd_pl_execute(&q, &context, lib, result);
			assert(result->error && atomic_load(&calls) == calls_before);
			free(result); nd_wipe(&context, sizeof(context));
		}
		old_upload.action(old_upload.user); assert(fixture_upload_creates == creates + 1);
		if (page == 1) lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
		else {
			int index = -1; for (int i = 0; i < pl_pages[0].count; i++) if (pl_pages[0].playlists[i].id == id) index = i;
			assert(index >= 0); lv_obj_send_event(pl_pages[0].menus[index], LV_EVENT_CLICKED, NULL);
		}
		assert(menu_index("nd_pl_upload") < 0);
		if (phase >= 2) assert(menu_index(phase == 2 ? "nd_pl_reconcile" : "nd_pl_verify") >= 0);
	}
	fixture_upload_name[0] = 0; fixture_upload_lost = fixture_upload_bad_verify = false;
	puts("playlist Upload UI/service: exact collection and detail targets, empty/repeated/no-Sync definitions, verified links, stale callbacks and unknown/known-ID recovery without blind creation: passed");
}
static void saved_playlist_taps(const nd_library_t *lib) {
	char path[1200];
	nd_playlist_store_t store;
	assert(nd_pl_path(lib, path) && nd_pl_open(&store, path, lib->scope, true));
	int64_t id;
	assert(nd_pl_create(&store, "Repeated", &id) && nd_pl_begin(&store, id, 1));
	int added = 0, skipped = 0;
	for (int i = 0; i < 40; i++) {
		nd_row_t row = {.title = "Repeated title", .cover = "repeated-cover"};
		snprintf(row.id, sizeof(row.id), "song-%05d", i % 2);
		assert(nd_pl_append(&store, id, &row, false, &added, &skipped));
	}
	nd_playlist_t p;
	assert(nd_pl_finish(&store, id, true) && nd_pl_sql(&store, "COMMIT") && nd_pl_get(&store, id, &p));
	nd_pl_close(&store);
	pl_open_detail(&p); pl_wait(1);
	fixture_art_ready = true;
	navidromeplaylists_refresh();
	pl_page_t *detail = &pl_pages[1];
	lv_obj_t *retained = detail->rows[12];
	void *retained_pixels = detail->artwork[12].pixels;
	int64_t retained_occurrence = detail->entries[12].occurrence;
	assert(retained_pixels);
	lv_obj_scroll_to_y(detail->list, detail->offset + 12 * detail->pitch, LV_ANIM_OFF);
	pl_wait(1);
	int match = -1;
	for (int i = 0; i < detail->count; i++)
		if (detail->entries[i].occurrence == retained_occurrence) match = i;
	assert(match >= 0 && detail->rows[match] == retained && detail->artwork[match].pixels == retained_pixels);
	assert_cover_rendered(detail->images[match]);
	lv_obj_scroll_to_y(detail->list, detail->offset + detail->total * detail->pitch, LV_ANIM_OFF);
	pl_wait(1);
	navidromeplaylists_refresh();
	unsigned edge_serial = detail->serial, edge_art = fixture_art_requests;
	for (int bounce = 0; bounce < 5; bounce++) {
		lv_obj_scroll_by(detail->list, 0, -detail->pitch / 2, LV_ANIM_OFF);
		assert(detail->serial == edge_serial && fixture_art_requests == edge_art);
		assert_cover_rendered(detail->images[detail->count - 1]);
		lv_obj_scroll_by(detail->list, 0, detail->pitch / 2, LV_ANIM_OFF);
	}
	lv_obj_scroll_to_y(detail->list, 0, LV_ANIM_OFF);
	pl_wait(1);
	navidromeplaylists_refresh();
	puts("repeated playlist occurrences: overlap retains exact occurrence widget/pixels, bottom bounce submits no query/artwork: passed");
	assert(!lv_obj_is_hidden(pl_pages[1].download) && !lv_obj_has_state(pl_pages[1].download, LV_STATE_DISABLED));
	lv_obj_update_layout(pl_pages[1].screen);
	lv_area_t download_bounds;
	lv_obj_get_coords(pl_pages[1].download, &download_bounds);
	assert(download_bounds.x1 >= ui_px(56) && download_bounds.x2 < ui_panel_width);
	for (int i = 0; i < 3; i++) {
		lv_area_t other;
		lv_obj_get_coords(pl_pages[1].buttons[i], &other);
		assert(download_bounds.x2 < other.x1);
	}
	lv_obj_t *glyph = lv_obj_get_child(pl_pages[1].download, 0);
	assert(icon_download.header.w > 1 && lv_image_get_src(glyph) == &icon_download);
	assert(lv_obj_get_width(glyph) == ui_px(24) && lv_obj_get_height(glyph) == ui_px(24));
	unsigned submitted = download_submissions;
	lv_obj_send_event(pl_pages[1].download, LV_EVENT_CLICKED, NULL);
	assert(download_submissions == submitted + 1 && captured_download.playlist == p.id && captured_download.playlist_revision == p.revision);
	assert(!strcmp(captured_download.destination, "Music/Navidrome") && pl_pages[1].download_pending);
	lv_obj_send_event(pl_pages[1].download, LV_EVENT_CLICKED, NULL);
	assert(download_submissions == submitted + 1);
	captured_download_cb(true, NULL, 2, 38, captured_download_user);
	assert(!pl_pages[1].download_pending);
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	assert(menu_index("nd_pl_download") >= 0 && menu_index("add_to_queue") >= 0 && menu_index("nd_pl_upload") >= 0);
	menu_activate("nd_pl_download");
	assert(download_submissions == submitted + 2 && pl_pages[1].download_pending);
	captured_download_cb(true, NULL, 2, 38, captured_download_user);
	submitted++;
	marker_library=*lib; snprintf(selected_track,sizeof(selected_track),"song-00000");
	navidromeplaylists_refresh();
	for(int row=0;row<pl_pages[1].count;row++) assert(lv_obj_is_hidden(pl_pages[1].playmarks[row])==!!strcmp(pl_pages[1].entries[row].track.id,selected_track));
	selected_track[0]=0; navidromeplaylists_refresh();
	for(int row=0;row<pl_pages[1].count;row++) assert(lv_obj_is_hidden(pl_pages[1].playmarks[row]));
	unsigned before = saved_taps;
	lv_obj_send_event(pl_pages[1].rows[2], LV_EVENT_CLICKED, NULL);
	assert(saved_taps == before + 1 && saved_playlist == id && saved_revision == p.revision && saved_ordinal == 2);
	assert(saved_occurrence == pl_pages[1].entries[2].occurrence && !strcmp(saved_track, "song-00000"));
	dragging = true;
	lv_obj_send_event(pl_pages[1].download, LV_EVENT_CLICKED, NULL);
	assert(download_submissions == submitted + 1);
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_CLICKED, NULL);
	dragging = false;
	assert(saved_taps == before + 1);
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_CLICKED, NULL); // delayed long-press release
	pl_wait(1);
	assert(pl_pages[1].selecting && saved_taps == before + 1);
	assert(lv_obj_is_hidden(pl_pages[1].download));
	lv_obj_send_event(pl_pages[1].download, LV_EVENT_CLICKED, NULL);
	assert(download_submissions == submitted + 1);
	lv_obj_send_event(pl_pages[1].rows[1], LV_EVENT_CLICKED, NULL); pl_wait(1);
	assert(saved_taps == before + 1 && pl_pages[1].selected == 2);
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL); pl_wait(1);
	assert(!pl_pages[1].selecting);
	lv_obj_scroll_to_y(pl_pages[1].list, pl_pages[1].offset + 34 * pl_pages[1].pitch, LV_ANIM_OFF);
	pl_load(1); pl_wait(1);
	int tapped_ordinal = pl_pages[1].first + 2;
	assert(tapped_ordinal > ND_WINDOW);
	lv_obj_send_event(pl_pages[1].rows[2], LV_EVENT_CLICKED, NULL);
	assert(saved_taps == before + 2 && saved_ordinal == tapped_ordinal && saved_occurrence == pl_pages[1].entries[2].occurrence);
	pl_pages[1].card++;
	lv_obj_send_event(pl_pages[1].rows[2], LV_EVENT_CLICKED, NULL);
	pl_pages[1].card--;
	assert(saved_taps == before + 2);
	lv_obj_send_event(pl_pages[1].download, LV_EVENT_CLICKED, NULL);
	assert(pl_pages[1].download_pending);
	unsigned leaves = saved_leaves;
	navidromeplaylists_open(); pl_wait(0);
	captured_download_cb(false, "nd_dl_capture_error", 0, 0, captured_download_user);
	assert(!pl_pages[1].download_pending && pl_active(0));
	assert(saved_leaves > leaves);
	puts("playlist LVGL ordinary/repeated/off-window occurrence taps, long-press/selection/Cancel, gestures, stale card and origin leave: passed");
}

#ifndef NAVIDROME_OFFLINE_UI
static void polish_browse_edges(void) {
	browse_page_t *p = &pages[0];
	assert(p->first == 0 && p->count == ND_WINDOW);
	navidromebrowse_refresh();
	lv_obj_t *widget = p->rows[12];
	void *pixels = p->artwork[12].pixels;
	assert(pixels);
	char id[ND_ID_MAX];
	snprintf(id, sizeof(id), "%s", p->data[12].id);
	lv_obj_scroll_to_y(p->list, 12 * pitch, LV_ANIM_OFF);
	window_wait(0);
	int found = -1;
	for (int i = 0; i < p->count; i++) if (!strcmp(p->data[i].id,id)) found=i;
#ifdef NAVIDROME_EDGE_BASELINE
	assert(found >= 0 && p->rows[found] != widget && !p->artwork[found].pixels);
	assert(lv_image_get_src(p->images[found]) == &icon_music_note);
	puts("baseline reproduced: a still-visible overlapping item loses its decoded cover and shows the placeholder after a window shift");
	exit(0);
#endif
	assert(found >= 0 && p->rows[found] == widget && p->artwork[found].pixels == pixels);
	assert_cover_rendered(p->images[found]);
	lv_obj_scroll_to_y(p->list, p->total * pitch, LV_ANIM_OFF);
	window_wait(0);
	navidromebrowse_refresh();
	assert(p->first + p->count == p->total);
	unsigned serial = p->serial, requests = fixture_art_requests;
	for (int i = 0; i < 5; i++) {
		lv_obj_scroll_by(p->list, 0, -pitch / 2, LV_ANIM_OFF);
		assert(p->serial == serial && fixture_art_requests == requests);
		for (int row = 0; row < p->count; row++)
			assert(p->artwork[row].pixels && lv_image_get_src(p->images[row]) == &p->artwork[row].dsc);
		lv_obj_scroll_by(p->list, 0, pitch / 2, LV_ANIM_OFF);
		assert(p->serial == serial);
	}
	lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
	window_wait(0);
	navidromebrowse_refresh();
	serial = p->serial; requests = fixture_art_requests;
	for (int i = 0; i < 5; i++) {
		lv_obj_scroll_by(p->list, 0, pitch / 2, LV_ANIM_OFF);
		assert(p->serial == serial && fixture_art_requests == requests);
		assert_cover_rendered(p->images[0]);
		lv_obj_scroll_by(p->list, 0, -pitch / 2, LV_ANIM_OFF);
	}
	puts("list cover pixels: overlapping rows retain widgets/pixels; repeated top/bottom elastic bounce makes no query or artwork request: passed");
}

int main(int argc, char **argv) {
	ui_panel_width = argc > 1 ? atoi(argv[1]) : 320;
	assert(ui_panel_width == 320 || ui_panel_width == 480);
	int panel_height = ui_panel_width == 320 ? 480 : 800;
	alarm(60);
	char dir[] = "/tmp/sonix-playlist-ui.XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	wait_idle();
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	seed(&lib);
	nd_store_t art_seed;
	assert(nd_store_open(&art_seed, lib.path, lib.scope, true, false));
	assert(sqlite3_exec(art_seed.db, "UPDATE entity SET cover='fixture-browse-cover' WHERE (kind=0 AND id='album-00000') OR kind=1", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&art_seed);
	navidrome_set_root(dir);
	library_wait(&lib);
	assert(lib.ready);
	lv_init();
	font_ui_28 = font_ui_24_bold = font_ui_14 = font_ui_16 = font_ui_18 = font_ui_20 = font_ui_24 = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(ui_panel_width, panel_height);
	lv_style_init(&theme_style_icon);
	lv_style_set_image_recolor_opa(&theme_style_icon, LV_OPA_COVER);
	navidrome_screen = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = (unsigned)ui_panel_width, .screen_height = (unsigned)panel_height, .padding = 8, .top_bar_height = 20};
	navidromealbum_set_enabled(false); // This fixture checks the retained thumbnail layout.
	navidromebrowse_init(&cfg);
	navidromeplaylists_init(&cfg);
	int requests = atomic_load(&calls);
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_active(0) && pl_pages[0].total == 0);
	assert(!lv_obj_is_hidden(pl_pages[0].new_row) && !lv_obj_is_hidden(pl_pages[0].empty));
	assert(!lv_obj_is_hidden(pl_pages[0].buttons[0]) && lv_obj_is_hidden(pl_pages[0].buttons[1]));
	assert(lv_obj_get_height(pl_pages[0].new_row) == ui_px(88));
	assert(pl_pages[0].pitch == ui_px(88) + ui_px(8));
	assert(lv_obj_get_style_radius(pl_pages[0].new_row, 0) == ui_px(12));
	assert(lv_obj_get_width(pl_pages[0].list) == (int)cfg.screen_width);
	assert(lv_obj_get_style_pad_left(pl_pages[0].list, 0) == cfg.padding);
	assert(lv_obj_get_y(pl_pages[0].list) == settingsrow_content_top(&cfg));
	lv_obj_send_event(pl_pages[0].new_row, LV_EVENT_CLICKED, NULL);
	assert(!lv_obj_is_hidden(pl_name_layer));
	assert(lv_obj_get_parent(pl_name_layer) == pl_pages[0].screen);
	assert(lv_obj_get_parent(pl_name_layer) != lv_layer_top());
	lv_obj_update_layout(pl_name_layer);
	lv_area_t heading_area, close_area;
	lv_obj_get_coords(lv_obj_get_child(pl_name_layer, 0), &heading_area);
	lv_obj_get_coords(lv_obj_get_child(pl_name_layer, -1), &close_area);
	assert(heading_area.x1 >= cfg.padding + ui_px(56) + ui_px(14));
	assert(heading_area.x2 < close_area.x1);
	assert(heading_area.y1 == cfg.top_bar_height + cfg.padding + ui_px(10));
	assert(lv_obj_get_height(pl_name_field) == ui_px(62));
	assert(lv_obj_get_style_text_font(pl_name_field, 0) == &font_ui_24);
	assert(lv_obj_has_state(pl_name_field, LV_STATE_FOCUSED));
	lv_textarea_set_text(pl_name_field, "Road playlist");
	pl_name_accept(NULL);
	pl_wait(1);
	assert(pl_active(1) && pl_pages[1].playlist.count == 0 && !strcmp(pl_pages[1].playlist.name, "Road playlist"));
	assert(!strcmp(lv_label_get_text(pl_pages[1].empty), "No tracks yet."));
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_CLICKED, NULL);
	assert(saved_taps == 0);
	// The detail Add tracks flow selects an album and returns to this exact playlist.
	lv_obj_send_event(pl_pages[1].buttons[2], LV_EVENT_CLICKED, NULL);
	assert(fixture_menu_count == 2);
	fixture_menu[1].action(fixture_menu[1].user);
	window_wait(0);
	assert(pages[0].query.kind == ND_ALBUMS);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	selection_wait();
	assert(pages[0].selected == 1);
	lv_obj_send_event(pages[0].select_add, LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_active(1) && pl_pages[1].playlist.count == 10);
	int64_t rev = pl_pages[1].playlist.revision;
	// Readding that album skips all tracks, without changing the revision.
	lv_obj_send_event(pl_pages[1].buttons[2], LV_EVENT_CLICKED, NULL);
	fixture_menu[1].action(fixture_menu[1].user);
	window_wait(0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	selection_wait();
	lv_obj_send_event(pages[0].select_add, LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 10 && pl_pages[1].playlist.revision == rev);
	// Detail selection removes only chosen occurrences in a confirmed local batch.
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	pl_wait(1);
	assert(pl_pages[1].selecting && pl_pages[1].selected == 1);
	lv_obj_send_event(pl_pages[1].rows[1], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].selected == 2);
	lv_obj_send_event(pl_pages[1].buttons[1], LV_EVENT_CLICKED, NULL);
	assert(fixture_confirm);
	fixture_confirm(fixture_confirm_user);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 8 && !pl_pages[1].selecting);
	// Header exposes an explicit Upload choice, never an implicit write on icon tap.
	lv_obj_send_event(pl_pages[1].buttons[1], LV_EVENT_CLICKED, NULL);
	assert(fixture_menu_count == 1 && !strcmp(fixture_menu[0].label, "nd_pl_upload"));
	// No local edit sent HTTP.
	assert(atomic_load(&calls) == requests);
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	assert(menu_index("rename") >= 0 && menu_index("nd_pl_download") >= 0 && menu_index("nd_pl_upload") >= 0);
	fixture_menu[0].action(NULL);
	lv_textarea_set_text(pl_name_field, "Renamed locally");
	pl_name_accept(NULL);
	pl_wait(1);
	assert(!strcmp(pl_pages[1].playlist.name, "Renamed locally"));
	// The collection and destination picker share Music's New playlist row,
	// track counts, card typography and row menu without a second header plus.
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 1 && lv_obj_is_hidden(pl_pages[0].empty));
	assert(lv_obj_get_y(pl_pages[0].rows[0]) == pl_pages[0].offset);
	assert(lv_obj_get_style_text_font(pl_pages[0].names[0], 0) == &font_ui_24);
	assert(lv_obj_get_style_text_font(pl_pages[0].details[0], 0) == &font_ui_16);
	assert(!strcmp(lv_label_get_text(pl_pages[0].details[0]), "8 tracks · nd_pl_local"));
	assert(lv_image_get_src(lv_obj_get_child(pl_pages[0].rows[0], 0)) == &icon_list_music);
	lv_area_t name_area, menu_area;
	lv_obj_get_coords(pl_pages[0].names[0], &name_area);
	lv_obj_get_coords(pl_pages[0].menus[0], &menu_area);
	assert(name_area.x1 < name_area.x2 && name_area.x2 < menu_area.x1);
	lv_obj_send_event(pl_pages[0].menus[0], LV_EVENT_CLICKED, NULL);
	assert(menu_index("rename") >= 0 && menu_index("nd_pl_delete_local") >= 0 && menu_index("nd_pl_upload") >= 0);
	lv_obj_send_event(pl_pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	navidromeplaylists_pick(9000, lib.generation, pl_pages[1].screen);
	pl_wait(2);
	assert(pl_active(2) && !lv_obj_is_hidden(pl_pages[2].new_row));
	assert(lv_obj_is_hidden(pl_pages[2].buttons[0]));
	lv_obj_send_event(pl_pages[2].new_row, LV_EVENT_CLICKED, NULL);
	assert(!lv_obj_is_hidden(pl_name_layer));
	assert(pl_back() && lv_obj_is_hidden(pl_name_layer));
	switch_screen_return_to(pl_pages[1].screen);
	pl_wait(1);
	assert(atomic_load(&calls) == requests);
	// Only playlist Add tracks changes a track tap into selection. The mode
	// propagates through album drill-down, and normal category entry clears it.
	unsigned played = playback_selections;
	lv_obj_send_event(pl_pages[1].buttons[2], LV_EVENT_CLICKED, NULL);
	fixture_menu[0].action(fixture_menu[0].user);
	window_wait(0);
	assert(pages[0].picking && pages[0].query.kind == ND_TRACKS);
	fixture_art_ready = true;
	navidromebrowse_refresh();
	assert_cover_rendered(pages[0].images[0]);
	unsigned art_requests = fixture_art_requests, art_releases = fixture_art_releases;
	void *cover_pixels = pages[0].artwork[0].pixels;
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(pages[0].selected == 1 && playback_selections == played);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && pages[0].artwork[0].pixels == cover_pixels);
	lv_obj_send_event(pages[0].rows[1], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(pages[0].selected == 2);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(pages[0].selected == 1 && !pages[0].chosen[0]);
	lv_obj_send_event(pages[0].rows[1], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(!pages[0].selecting && pages[0].selected == 0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(browse_selection_back());
	window_wait(0);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && pages[0].artwork[0].pixels == cover_pixels);
	assert_cover_rendered(pages[0].images[0]);
	switch_screen_return_to(pl_pages[1].screen);
	pl_wait(1);
	lv_obj_send_event(pl_pages[1].buttons[2], LV_EVENT_CLICKED, NULL);
	fixture_menu[1].action(fixture_menu[1].user);
	window_wait(0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	window_wait(1);
	assert(pages[1].picking && pages[1].query.kind == ND_TRACKS);
	fixture_art_ready = false;
	art_requests = fixture_art_requests;
	art_releases = fixture_art_releases;
	assert(fixture_art[fixture_browse_slot(&pages[1], 0)].pending);
	lv_obj_send_event(pages[1].rows[0], LV_EVENT_CLICKED, NULL);
	pick_wait(1);
	assert(pages[1].selected == 1 && playback_selections == played);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && fixture_art[fixture_browse_slot(&pages[1], 0)].pending);
	assert(browse_selection_back());
	window_wait(1);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && fixture_art[fixture_browse_slot(&pages[1], 0)].pending);
	fixture_art_ready = true;
	navidromebrowse_refresh();
	assert_cover_rendered(pages[1].images[0]);
	navidromebrowse_open(ND_TRACKS);
	window_wait(0);
	assert(!pages[0].picking);
	fixture_art_ready = true;
	navidromebrowse_refresh();
	assert_cover_rendered(pages[0].images[0]);
	polish_browse_edges();
	// Retention must not keep a cover when the same row gets a new cover ID.
	art_requests = fixture_art_requests;
	assert(nd_store_open(&art_seed, lib.path, lib.scope, true, false));
	assert(sqlite3_exec(art_seed.db, "UPDATE entity SET cover='fixture-replacement-cover' WHERE kind=1 AND id='song-00000'", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&art_seed);
	submit_window(&pages[0]);
	window_wait(0);
	assert(fixture_art_requests == art_requests + 1 && !strcmp(fixture_art[fixture_browse_slot(&pages[0], 0)].id, "fixture-replacement-cover"));
	assert(!pages[0].artwork[0].pixels && lv_image_get_src(pages[0].images[0]) == &icon_music_note);
	navidromebrowse_refresh();
	assert_cover_rendered(pages[0].images[0]);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == ++played);
	navidromebrowse_open(ND_ALBUMS);
	window_wait(0);
	navidromebrowse_refresh();
	assert_cover_rendered(pages[0].images[0]);
	art_requests = fixture_art_requests;
	art_releases = fixture_art_releases;
	cover_pixels = pages[0].artwork[0].pixels;
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	pick_wait(0);
	assert(pages[0].selected == 1);
	assert(browse_selection_back());
	window_wait(0);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && pages[0].artwork[0].pixels == cover_pixels);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	window_wait(1);
	assert(!pages[1].picking);
	navidromebrowse_refresh();
	assert_cover_rendered(pages[1].images[0]);
	lv_obj_send_event(pages[1].rows[0], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == ++played);
	switch_screen_return_to(pl_pages[1].screen);
	pl_wait(1);
	// Back cancels selection on this page, and gestures cannot select a row.
	dragging = true;
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	assert(!pl_pages[1].selecting);
	dragging = false;
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	pl_wait(1);
	assert(pl_back());
	pl_wait(1);
	assert(!pl_pages[1].selecting);
	// Exercise real LVGL pointer long-press and release, with the worker completing while held.
	lv_indev_t *pointer = lv_indev_create();
	lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_display(pointer, display);
	lv_indev_set_read_cb(pointer, fixture_touch_read);
	lv_indev_set_long_press_time(pointer, 50);
	lv_obj_update_layout(pl_pages[1].screen);
	lv_area_t hit;
	lv_obj_get_coords(pl_pages[1].rows[0], &hit);
	fixture_touch.point = (lv_point_t){.x = hit.x1 + 10, .y = hit.y1 + 10};
	fixture_touch.state = LV_INDEV_STATE_PRESSED;
	lv_indev_read(pointer);
	lv_tick_inc(80);
	lv_indev_read(pointer);
	pl_wait(1);
	assert(pl_pages[1].selecting && pl_pages[1].selected == 1);
	fixture_touch.state = LV_INDEV_STATE_RELEASED;
	lv_indev_read(pointer);
	pl_wait(1);
	assert(pl_pages[1].selecting && pl_pages[1].selected == 1);
	assert(pl_back());
	pl_wait(1);
	lv_indev_delete(pointer);
	// A dropped GUI completion recovers through a local read without replaying a write.
	int calls_before = atomic_load(&calls);
	pl_pages[1].loading = true;
	pl_pages[1].submitted = lv_tick_get() - 3000;
	pl_tick(pl_timer);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 8 && atomic_load(&calls) == calls_before);
	// Five thousand local entries use the same fixed visible pool and reach their final row.
	nd_playlist_t saved = pl_pages[1].playlist, big;
	nd_playlist_store_t store;
	char store_path[1200];
	assert(nd_pl_path(&lib, store_path));
	assert(nd_pl_open(&store, store_path, lib.scope, true));
	int64_t big_id;
	assert(nd_pl_create(&store, "Large local", &big_id) && nd_pl_begin(&store, big_id, 1));
	int added = 0, skipped = 0;
	nd_row_t row = {.title = "Bulk track"};
	for (int i = 0; i < 5000; i++) {
		snprintf(row.id, sizeof(row.id), "bulk-%05d", i);
		assert(nd_pl_append(&store, big_id, &row, false, &added, &skipped));
	}
	assert(nd_pl_finish(&store, big_id, true) && nd_pl_sql(&store, "COMMIT") && nd_pl_get(&store, big_id, &big));
	nd_pl_close(&store);
	pl_open_detail(&big);
	pl_wait(1);
	assert(pl_pages[1].count == ND_WINDOW && pl_pages[1].total == 5000);
	lv_obj_scroll_to_y(pl_pages[1].list, 4998 * pl_pages[1].pitch, LV_ANIM_OFF);
	lv_obj_update_layout(pl_pages[1].screen);
	lv_obj_send_event(pl_pages[1].list, LV_EVENT_SCROLL, NULL);
	pl_wait(1);
	assert(pl_pages[1].first + pl_pages[1].count == 5000 && !strcmp(pl_pages[1].entries[pl_pages[1].count - 1].track.id, "bulk-04999"));
	assert(nd_pl_open(&store, store_path, lib.scope, true) && nd_pl_edit(&store, big_id, big.revision, NULL, true) && nd_pl_sql(&store, "COMMIT"));
	nd_pl_close(&store);
	// The leading New playlist row must not shift window ordinals or hide
	// the last playlist when the collection exceeds the recycled row pool.
	assert(nd_pl_open(&store, store_path, lib.scope, true) && nd_pl_sql(&store, "BEGIN IMMEDIATE"));
	for (int i = 0; i < 40; i++) {
		char name[40];
		int64_t window_id;
		snprintf(name, sizeof(name), "Window %02d", i);
		assert(nd_pl_create(&store, name, &window_id));
	}
	assert(nd_pl_sql(&store, "COMMIT"));
	nd_pl_close(&store);
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 41 && pl_pages[0].count == ND_WINDOW);
	lv_obj_scroll_to_y(pl_pages[0].list, pl_pages[0].offset + 40 * pl_pages[0].pitch, LV_ANIM_OFF);
	lv_obj_update_layout(pl_pages[0].screen);
	lv_obj_send_event(pl_pages[0].list, LV_EVENT_SCROLL, NULL);
	pl_wait(0);
	assert(pl_pages[0].first + pl_pages[0].count == 41);
	assert(!strcmp(pl_pages[0].playlists[pl_pages[0].count - 1].name, "Window 39"));
	assert(nd_pl_open(&store, store_path, lib.scope, true));
	assert(nd_pl_sql(&store, "DELETE FROM playlist WHERE name LIKE 'Window %'"));
	nd_pl_close(&store);
	pl_open_detail(&saved);
	pl_wait(1);
	// Real asynchronous catalogue/import jobs: returning from an imported
	// detail reloads the saved catalogue, with no discovery request or stuck busy label.
	navidromeplaylists_open();
	pl_wait(0);
	lv_obj_send_event(pl_pages[0].buttons[0], LV_EVENT_CLICKED, NULL);
	pl_wait(3);
	assert(pl_pages[3].count == 3);
	lv_obj_send_event(pl_pages[3].rows[remote_index("fixture-empty")], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 0 && !strcmp(lv_label_get_text(pl_pages[1].empty), "No tracks yet."));
	int after_import = atomic_load(&calls);
	switch_screen_return_to(pl_pages[3].screen);
	pl_wait(3);
	assert(pl_pages[3].count == 3 && !lv_obj_is_hidden(pl_pages[3].rows[0]));
	assert(strcmp(lv_label_get_text(pl_pages[3].status), "nd_working") && atomic_load(&calls) == after_import);
	lv_obj_send_event(pl_pages[3].rows[remote_index("fixture-art")], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	nd_playlist_t linked = pl_pages[1].playlist;
	assert(linked.count == 2 && *linked.remote);
	navidromeplaylists_open();
	pl_wait(0);
	int linked_index = -1;
	for (int i = 0; i < pl_pages[0].count; i++)
		if (pl_pages[0].playlists[i].id == linked.id) linked_index = i;
	assert(linked_index >= 0);
	lv_obj_send_event(pl_pages[0].menus[linked_index], LV_EVENT_CLICKED, NULL);
	assert(menu_index("nd_pl_save_copy") >= 0);
	lv_obj_send_event(pl_pages[0].rows[linked_index], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(fixture_art_requests && !strcmp(fixture_art[pl_pages[1].art_slots[0]].id, "remote-cover"));
	assert(lv_obj_get_width(pl_pages[1].images[0]) == ui_px(ND_ART_THUMB_SIZE));
	assert(lv_image_get_inner_align(pl_pages[1].images[0]) == LV_IMAGE_ALIGN_CONTAIN);
	assert(lv_obj_get_width(lv_obj_get_parent(pl_pages[1].images[0])) == ui_px(ND_ART_THUMB_SIZE));
	assert(lv_obj_get_width(pl_pages[1].images[1]) == ui_px(ND_ART_THUMB_SIZE));
	lv_area_t art_area, track_area, text_area;
	lv_obj_get_coords(pl_pages[1].images[1], &art_area);
	lv_obj_get_coords(pl_pages[1].rows[1], &track_area);
	lv_obj_get_coords(pl_pages[1].names[1], &text_area);
	assert(art_area.y1 >= track_area.y1 && art_area.y2 <= track_area.y2);
	assert(art_area.x2 < text_area.x1);
	fixture_art_ready = true;
	navidromeplaylists_refresh();
	lv_obj_update_layout(pl_pages[1].screen);
	assert(lv_image_get_src(pl_pages[1].images[0]) == &pl_pages[1].artwork[0].dsc);
	assert(lv_obj_get_width(pl_pages[1].images[0]) == ui_px(ND_ART_THUMB_SIZE));
	assert_cover_rendered(pl_pages[1].images[0]);
	assert(lv_obj_get_style_image_recolor_opa(pl_pages[1].images[0], 0) == LV_OPA_TRANSP);
	assert(lv_image_get_src(pl_pages[1].images[1]) == &icon_music_note);
	art_requests = fixture_art_requests;
	art_releases = fixture_art_releases;
	cover_pixels = pl_pages[1].artwork[0].pixels;
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	pl_wait(1);
	lv_obj_send_event(pl_pages[1].rows[1], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].selected == 2);
	assert(pl_back());
	pl_wait(1);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && pl_pages[1].artwork[0].pixels == cover_pixels);
	assert_cover_rendered(pl_pages[1].images[0]);
	// Copy asks first; Cancel creates nothing. Accepting the unchanged name
	// makes a distinct unlinked copy, while a chosen name is saved atomically.
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	assert(menu_index("nd_pl_save_copy") >= 0);
	menu_activate("nd_pl_save_copy");
	assert(!lv_obj_is_hidden(pl_name_layer) && pl_copying);
	assert(lv_obj_get_parent(pl_name_layer) == pl_pages[1].screen);
	assert(!strcmp(lv_textarea_get_text(pl_name_field), linked.name));
	assert(pl_back());
	assert(nd_pl_open(&store, store_path, lib.scope, false));
	nd_playlist_t list_rows[ND_WINDOW];
	int listed, total_before_copy;
	assert(nd_pl_list(&store, 0, list_rows, &listed, &total_before_copy) && total_before_copy == 3);
	nd_pl_close(&store);
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	menu_activate("nd_pl_save_copy");
	pl_name_accept(NULL);
	pl_wait(1);
	assert(pl_pages[1].playlist.id != linked.id && !*pl_pages[1].playlist.remote);
	assert(!strcmp(pl_pages[1].playlist.name, linked.name) && pl_pages[1].playlist.count == 2);
	pl_open_detail(&linked);
	pl_wait(1);
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	menu_activate("nd_pl_save_copy");
	lv_textarea_set_text(pl_name_field, "Named local copy");
	pl_name_accept(NULL);
	pl_wait(1);
	assert(!*pl_pages[1].playlist.remote && !strcmp(pl_pages[1].playlist.name, "Named local copy"));
	assert(nd_pl_open(&store, store_path, lib.scope, false));
	nd_playlist_t unchanged;
	assert(nd_pl_get(&store, linked.id, &unchanged) && !strcmp(unchanged.name, linked.name) && !strcmp(unchanged.remote, linked.remote));
	nd_pl_close(&store);
	assert(atomic_load(&calls) == after_import + 1);
	// Leaving during a delayed import defers only the local collection read;
	// the accepted import finishes without pulling the user back to its detail.
	switch_screen_return_to(pl_pages[3].screen);
	pl_wait(3);
	for (int i = 0; i < ND_WINDOW; i++) assert(!fixture_art[i].pending);
	atomic_store(&fixture_delay_import, true);
	lv_obj_send_event(pl_pages[3].rows[remote_index("fixture-delayed")], LV_EVENT_CLICKED, NULL);
	for (int i = 0; i < 10000 && !atomic_load(&fixture_import_entered); i++) usleep(1000);
	assert(atomic_load(&fixture_import_entered));
	switch_screen_return_to(pl_pages[0].screen);
	assert(pl_pages[0].reload && !pl_pages[0].loading);
	atomic_store(&fixture_finish_import, true);
	for (int i = 0; i < 10000; i++) {
		drain();
		pl_tick(pl_timer);
		if (!navidrome_playlists_busy() && !pl_pages[0].reload && !pl_pages[0].loading) break;
		usleep(1000);
	}
	pl_wait(0);
	assert(pl_active(0) && pl_pages[0].total == 6 && !lv_obj_is_hidden(pl_pages[0].rows[0]));
	assert(nd_pl_open(&store, store_path, lib.scope, true));
	sqlite3_stmt *cleanup;
	assert(nd_pl_prepare(&store, &cleanup, "DELETE FROM playlist WHERE id<>?"));
	sqlite3_bind_int64(cleanup, 1, saved.id);
	assert(sqlite3_step(cleanup) == SQLITE_DONE);
	sqlite3_finalize(cleanup);
	nd_pl_close(&store);
	pl_open_detail(&saved);
	pl_wait(1);
	// Local lists remain available without a library snapshot and offline.
	assert(unlink(lib.path) == 0);
	navidrome_set_root(dir);
	library_wait(&lib);
	assert(!lib.ready);
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 1);
	lv_obj_send_event(pl_pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 8);
	atomic_store(&mode, 6);
	assert(navidrome_reconnect());
	wait_idle();
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 1);
	// Read-only card blocks writes while retaining readable playlist contents.
	assert(chmod(dir, 0500) == 0);
	navidrome_set_root(dir);
	library_wait(&lib);
	assert(!lib.writable);
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 1);
	pl_name_show(0, false, NULL);
	lv_textarea_set_text(pl_name_field, "Cannot write");
	pl_name_accept(NULL);
	pl_wait(0);
	assert(pl_pages[0].total == 1);
	assert(chmod(dir, 0700) == 0);
	navidrome_set_root(dir);
	library_wait(&lib);
	saved_playlist_taps(&lib);
	playlist_actions_ui(&lib);
	playlist_upload_ui(&lib);
	navidrome_lock();
	navidromeplaylists_refresh();
	assert(lv_screen_active() == navidrome_screen);
	navidrome_set_root(dir);
	navidrome_reset();
	drain();
	lv_display_delete(display);
	lv_deinit();
	char path[1200];
	snprintf(path, sizeof(path), "%s/.local/navidrome/.cache", dir);
	assert(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/.local/navidrome", dir);
	assert(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/.local", dir);
	assert(rmdir(path) == 0);
	assert(!chdir("/") && rmdir(dir) == 0);
	printf("Music-style playlists at %dx%d: cards, scoped picking, naming/Back, bounded art, import return and named copies passed\n", ui_panel_width, panel_height);
	puts("real LVGL playlist creation, empty Add tracks, album expansion/dedupe, selection/removal, rename, gestures, offline/no-snapshot/read-only/lock: passed");
}

#endif
