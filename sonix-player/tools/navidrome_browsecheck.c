// Real LVGL lists, background window reads, SQLite, and the credential service.
#define gui_post unused_fixture_post
#define main unused_service_main
#include "navidrome_selftest.c"
#undef main
#undef gui_post
#include "src/gui/shell/azindex.c"
#include "src/gui/streaming/navidromebrowse.c"

int32_t ui_panel_width = 320;
lv_font_t font_ui_14, font_ui_18, font_ui_20, font_ui_64_bold;
extern const lv_font_t lv_font_unscii_8;
lv_style_t theme_style_icon;
static const uint8_t fixture_pixel[4] = {255, 255, 255, 255};
const lv_image_dsc_t icon_music_note = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 1, .h = 1, .stride = 4}, .data = fixture_pixel, .data_size = 4};
lv_style_t theme_style_screen, theme_style_card, theme_style_card_pressed, theme_style_text, theme_style_text_dim;
lv_obj_t *navidrome_screen;
static bool dragging;
const char *tr(const char *key) { return key; }
void gui_notify_popup(const char *key) { (void)key; }
bool switcher_back_drag_active(void) { return dragging; }
bool player_sheet_drag_active(void) { return dragging; }
bool coverflow_drag_active(void) { return dragging; }
void switcher_attach_back_gesture(lv_obj_t *object) { (void)object; }
void player_sheet_attach_drag(lv_obj_t *object, bool opening) {
	(void)object;
	(void)opening;
}
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_return_to(lv_obj_t *screen) { lv_screen_load(screen); }
void thread_be_background(const char *name) { (void)name; }
int settingsrow_content_top(gui_config_t *cfg) {
	(void)cfg;
	return 65;
}
lv_obj_t *settingsrow_title(lv_obj_t *screen, gui_config_t *cfg, const char *text) {
	(void)cfg;
	lv_obj_t *label = lv_label_create(screen);
	lv_label_set_text(label, text);
	return label;
}
void navidromeart_release(int slot) { (void)slot; }
void navidromeart_request(int slot, const nd_library_t *library, const char *id, int size) {
	(void)slot;
	(void)library;
	(void)id;
	(void)size;
}
bool navidromeart_take(int slot, cover_image_t *image, bool *finished) {
	(void)slot;
	(void)image;
	*finished = true;
	return false;
}
void cover_free(cover_image_t *image) {
	free(image->pixels);
	memset(image, 0, sizeof(*image));
}
void frameprof_begin(const char *tag) { (void)tag; }
void frameprof_end(const char *tag) { (void)tag; }
bool frameprof_timing(void) { return false; }
unsigned long long frameprof_now_us(void) { return 0; }
void frameprof_timer_ran(void *cb, unsigned us) {
	(void)cb;
	(void)us;
}
void frameprof_event_ran(void *cb, unsigned us) {
	(void)cb;
	(void)us;
}

const theme_palette_t *theme(void) {
	static theme_palette_t palette;
	return &palette;
}
void theme_register_refresh(theme_refresh_cb_t cb) { (void)cb; }
void power_pause_in_standby(lv_timer_t *timer) { (void)timer; }

static pthread_mutex_t post_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	void (*callback)(void *);
	void *data;
} posts[64];
static unsigned post_read, post_write;
bool gui_post(void (*cb)(void *), void *data) {
	pthread_mutex_lock(&post_lock);
	bool ok = post_write - post_read < 64;
	if (ok) {
		posts[post_write % 64].callback = cb;
		posts[post_write++ % 64].data = data;
	}
	pthread_mutex_unlock(&post_lock);
	return ok;
}
static void drain(void) {
	for (;;) {
		pthread_mutex_lock(&post_lock);
		if (post_read == post_write) {
			pthread_mutex_unlock(&post_lock);
			return;
		}
		void (*callback)(void *) = posts[post_read % 64].callback;
		void *data = posts[post_read++ % 64].data;
		pthread_mutex_unlock(&post_lock);
		callback(data);
	}
}
static void window_wait(int level) {
	for (int i = 0; i < 10000; i++) {
		drain();
		lv_obj_update_layout(pages[level].screen);
		if (!pages[level].loading && pages[level].count)
			return;
		usleep(1000);
	}
	assert(!"window timeout");
}

static void seed(const nd_library_t *library) {
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s", library->path);
	*strrchr(dir, '/') = 0;
	char parent[1100];
	snprintf(parent, sizeof(parent), "%s", dir);
	*strrchr(parent, '/') = 0;
	assert(!mkdir(parent, 0700) || errno == EEXIST);
	assert(!mkdir(dir, 0700) || errno == EEXIST);
	nd_store_t store;
	assert(nd_store_open(&store, library->path, library->scope, true, false));
	assert(nd_store_begin(&store));
	assert(sqlite3_exec(store.db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_stmt *st;
	assert(sqlite3_prepare_v2(store.db, "INSERT INTO entity VALUES(?,?,?,?,'Fixture',?,'',120,?,?,?)", -1, &st, NULL) == SQLITE_OK);
	for (int kind = 0; kind < 4; kind++) {
		int total = kind == ND_TRACKS ? 17000 : kind == ND_ALBUMS ? 1700 : 1;
		for (int i = 0; i < total; i++) {
			char id[64], title[64], album[64] = "";
			snprintf(id, sizeof(id), kind == ND_TRACKS ? "song-%05d" : kind == ND_ALBUMS ? "album-%05d" : kind == ND_ARTISTS ? "artist-%05d" : "genre-%05d", i);
			if (kind == ND_TRACKS)
				snprintf(title, sizeof(title), "%c Track %05d", 'A' + i / 654, i);
			else
				snprintf(title, sizeof(title), "%s %05d", kind == ND_ALBUMS ? "Album" : kind == ND_ARTISTS ? "Artist" : "Genre", i);
			if (kind == ND_TRACKS)
				snprintf(album, sizeof(album), "album-%05d", i / 10);
			sqlite3_reset(st);
			sqlite3_bind_int64(st, 1, store.generation);
			sqlite3_bind_int(st, 2, kind);
			sqlite3_bind_text(st, 3, id, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(st, 4, title, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(st, 5, album, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(st, 6, i % 2);
			sqlite3_bind_int(st, 7, i % 10);
			char key[LIBRARY_SORT_KEY_MAX];
			library_sort_key_ex(title, key, sizeof(key), true);
			sqlite3_bind_text(st, 8, key, -1, SQLITE_TRANSIENT);
			assert(sqlite3_step(st) == SQLITE_DONE);
		}
	}
	sqlite3_finalize(st);
	for (int i = 0; i < 1700; i++) {
		char text[256];
		snprintf(text, sizeof(text), "INSERT INTO rel VALUES(%lld,'album-%05d',2,'artist-00000'); INSERT INTO rel VALUES(%lld,'album-%05d',3,'genre-00000');", (long long)store.generation, i, (long long)store.generation, i);
		assert(sqlite3_exec(store.db, text, NULL, NULL, NULL) == SQLITE_OK);
	}
	assert(sqlite3_exec(store.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
	assert(nd_store_validate(&store) && nd_store_publish(&store));
	nd_store_close(&store);
}

int main(void) {
	alarm(60);
	setvbuf(stdout, NULL, _IONBF, 0);
	char dir[] = "/tmp/sonix-navidrome-browse-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	wait_idle();
	nd_library_t library;
	navidrome_library_snapshot(&library);
	assert(library.writable);
	seed(&library);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&library);
		if (!library.loading)
			break;
		usleep(1000);
	}
	assert(library.ready);
	lv_init();
	font_ui_14 = font_ui_18 = font_ui_20 = font_ui_64_bold = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(320, 480);
	navidrome_screen = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = 320, .screen_height = 480, .padding = 8, .top_bar_height = 20};
	navidromebrowse_init(&cfg);
	int requests_before = atomic_load(&calls);
	navidromebrowse_open(ND_TRACKS);
	window_wait(0);
	assert(pages[0].total == 17000 && pages[0].count == ND_WINDOW);
	lv_obj_scroll_to_y(pages[0].list, 16995 * pitch, LV_ANIM_OFF);
	lv_obj_update_layout(pages[0].screen);
	lv_obj_send_event(pages[0].list, LV_EVENT_SCROLL, NULL);
	window_wait(0);
	assert(pages[0].first > 16980 && pages[0].first + pages[0].count == 17000);
	assert(!strcmp(pages[0].data[pages[0].count - 1].title, "Z Track 16999"));
	puts("real LVGL windowed scrolling reaches track 17000: passed");
	assert(pages[0].index->enabled && pages[0].letters[26] == 25 * 654);
	assert(pages[0].index->row[0] == 0 && pages[0].index->row[27] == 25 * 654);
	int before_strip_gesture = lv_obj_get_scroll_y(pages[0].list);
	dragging = true;
	lv_obj_send_event(pages[0].index->bar, LV_EVENT_PRESSED, NULL);
	lv_obj_send_event(pages[0].index->bar, LV_EVENT_PRESSING, NULL);
	lv_obj_send_event(pages[0].index->bar, LV_EVENT_RELEASED, NULL);
	assert(lv_obj_get_scroll_y(pages[0].list) == before_strip_gesture);
	dragging = false;
	go(pages[0].index, 2);
	go(pages[0].index, 13);
	go(pages[0].index, 26); // Newest jump supersedes any older in-flight window.
	window_wait(0);
	assert(pages[0].first <= 25 * 654 && pages[0].first + pages[0].count > 25 * 654);
	azindex_flash(pages[0].index);
	assert(!lv_obj_is_hidden(pages[0].index->bar));
	lv_tick_inc(2600);
	timer_cb(timer);
	assert(lv_obj_is_hidden(pages[0].index->bar) && lv_obj_is_hidden(pages[0].index->hint));
	azindex_set_rows(pages[0].index, pages[0].letters, 29, false);
	assert(!pages[0].index->enabled);
	azindex_set_rows(pages[0].index, pages[0].letters, 30, false);
	assert(pages[0].index->enabled);
	puts("real letter strip: full-library Z jump, rapid coalescing, 29/30 eligibility and inactivity hide: passed");

	navidromebrowse_open(ND_ALBUMS);
	window_wait(0);
	lv_obj_scroll_to_y(pages[0].list, 100 * pitch, LV_ANIM_OFF);
	lv_obj_update_layout(pages[0].screen);
	lv_obj_send_event(pages[0].list, LV_EVENT_SCROLL, NULL);
	window_wait(0);
	int saved_scroll = lv_obj_get_scroll_y(pages[0].list);
	dragging = true;
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == pages[0].screen);
	dragging = false;
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	window_wait(1);
	assert(pages[1].total == 10 && !pages[1].index->enabled);
	switch_screen_return_to(pages[0].screen);
	window_wait(0);
	assert(lv_obj_get_scroll_y(pages[0].list) == saved_scroll);
	for (int kind = ND_ARTISTS; kind <= ND_GENRES; kind++) {
		navidromebrowse_open(kind);
		window_wait(0);
		lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
		window_wait(1);
		assert(pages[1].total == 1700 && pages[1].index->enabled);
		assert(pages[1].letters[1] == 0); // The full matching artist/genre album query.
		lv_obj_send_event(pages[1].rows[0], LV_EVENT_CLICKED, NULL);
		window_wait(2);
		assert(pages[2].total == 10 && !pages[2].index->enabled);
	}
	// Publishing a snapshot that removes the selected album walks back to a
	// valid root rather than retaining a stale ID or mixed-generation rows.
	nd_store_t replacement;
	assert(nd_store_open(&replacement, library.path, library.scope, true, false));
	assert(nd_store_begin(&replacement) && nd_store_validate(&replacement) && nd_store_publish(&replacement));
	nd_store_close(&replacement);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&library);
		if (!library.loading)
			break;
		usleep(1000);
	}
	navidromebrowse_refresh();
	for (int i = 0; i < 10000; i++) {
		drain();
		if (lv_screen_active() == pages[0].screen && !pages[0].loading)
			break;
		usleep(1000);
	}
	assert(lv_screen_active() == pages[0].screen && pages[0].total == 0);
	seed(&library);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&library);
		if (!library.loading)
			break;
		usleep(1000);
	}
	navidromebrowse_open(ND_TRACKS);
	window_wait(0);
	puts("real LVGL snapshot replacement and removed-selection recovery: passed");
	assert(atomic_load(&calls) == requests_before);
	puts("real LVGL four paths, parent scroll, gesture guards, no network/playback: passed");
	navidromebrowse_open(ND_TRACKS); // Leave one window completion pending.
	navidrome_lock();
	navidromebrowse_refresh();
	drain();
	assert(lv_screen_active() == navidrome_screen && !pages[0].index->enabled);
	atomic_store(&mode, 6);
	assert(navidrome_unlock("vault1"));
	wait_idle();
	navidromebrowse_open(ND_TRACKS);
	window_wait(0);
	assert(pages[0].total == 17000);
	navidrome_set_root(NULL);
	navidromebrowse_refresh();
	assert(lv_screen_active() == navidrome_screen && !pages[0].index->enabled);
	puts("real LVGL lock, offline unlock and card invalidation: passed");
	navidrome_set_root(dir);
	navidrome_reset();
	drain();
	lv_display_delete(display);
	lv_deinit();
	char path[1200];
	snprintf(path, sizeof(path), "%s/.local/navidrome", dir);
	assert(!rmdir(path));
	snprintf(path, sizeof(path), "%s/.local", dir);
	assert(!rmdir(path));
	assert(!chdir("/") && !rmdir(dir));
	return 0;
}
