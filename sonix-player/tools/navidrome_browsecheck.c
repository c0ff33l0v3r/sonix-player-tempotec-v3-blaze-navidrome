// Real LVGL lists, background window reads, SQLite, and the credential service.
#define gui_post unused_fixture_post
#define main unused_service_main
#include "navidrome_selftest.c"
#undef main
#undef gui_post
#include "src/gui/shell/azindex.c"
#ifdef icon_download
#undef icon_download
extern const lv_image_dsc_t icon_download;
#include "src/gui/streaming/navidromeofflinebadge.c"
#define icon_download fixture_download_stub
extern const lv_image_dsc_t fixture_download_stub;
#else
#include "src/gui/streaming/navidromeofflinebadge.c"
#endif
#include "src/gui/streaming/navidromepage.h"
#ifndef NAVIDROME_BROWSE_SOURCE
#define NAVIDROME_BROWSE_SOURCE "src/gui/streaming/navidromebrowse.c"
#endif
#include NAVIDROME_BROWSE_SOURCE
#include "src/gui/streaming/navidromealbum.c"

int32_t ui_panel_width = 320;
lv_font_t font_ui_14, font_ui_18, font_ui_20, font_ui_64_bold, font_ui_24, font_ui_28, font_ui_24_bold;
extern const lv_font_t lv_font_unscii_8;
lv_style_t theme_style_icon;
static const uint8_t fixture_pixel[4] = {255, 255, 255, 255};
#define FIXTURE_ICON {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 1, .h = 1, .stride = 4}, .data = fixture_pixel, .data_size = 4}
const lv_image_dsc_t icon_download = FIXTURE_ICON, icon_album_big = FIXTURE_ICON, icon_shuffle = FIXTURE_ICON, icon_list_queue = FIXTURE_ICON, icon_star_plus = FIXTURE_ICON, icon_close = FIXTURE_ICON, icon_list_plus = FIXTURE_ICON, icon_ellipsis_vertical = FIXTURE_ICON, icon_check = FIXTURE_ICON;
#ifndef NAVIDROME_PLAYLIST_UI
void navidromeplaylists_pick_source(int64_t session, int64_t generation, lv_obj_t *origin, bool favorites) {
	(void)favorites;
	navidromeplaylists_pick(session, generation, origin);
}
void navidromeplaylists_pick(int64_t session, int64_t generation, lv_obj_t *origin) {
	(void)session;
	(void)generation;
	(void)origin;
}
#endif
void settingsrow_title_fit_width(lv_obj_t *title, int width) {
	lv_obj_set_width(title, width);
}
void settingsrow_title_corner_slots(lv_obj_t *title, gui_config_t *cfg, int n) {
	(void)title;
	(void)cfg;
	(void)n;
}
void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	(void)screen;
	(void)handler;
}
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*handler)(void)) {
	(void)screen;
	(void)handler;
}
popover_item_t fixture_menu[7];
int fixture_menu_count;
void popover_close(void) {}
void popover_show(lv_obj_t *anchor, const popover_item_t *items, int count) {
	(void)anchor;
	fixture_menu_count = count;
	memcpy(fixture_menu, items, (size_t)count * sizeof(*items));
}
static const uint8_t fixture_note_pixels[128 * 128 * 4];
const lv_image_dsc_t icon_music_note = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 128, .h = 128, .stride = 512}, .data = fixture_note_pixels, .data_size = sizeof(fixture_note_pixels)};
lv_style_t theme_style_screen, theme_style_card, theme_style_card_pressed, theme_style_text, theme_style_text_dim;
lv_obj_t *navidrome_screen;
#ifndef NAVIDROME_STAR_UI
lv_obj_t *navidromefavorites_page(void) { return navidrome_screen; }
#endif
static bool dragging;
#ifndef NAVIDROME_COORDINATOR_TEST
static unsigned playback_selections;
static char selected_track[ND_ID_MAX], selected_album[ND_ID_MAX], queued_track[ND_ID_MAX];
static nd_library_t marker_library;
static unsigned fixture_queue_adds;
static int64_t queued_session;
bool navidromeplay_select(const nd_library_t *library, const nd_query_t *query, const char *id, void *origin) {
	assert(library->ready && query->kind == ND_TRACKS && origin == lv_screen_active());
	snprintf(selected_track, sizeof(selected_track), "%s", id);
	marker_library = *library;
	snprintf(selected_album,sizeof(selected_album),"%s", query->filter_kind==ND_ALBUMS?query->filter:"");
	playback_selections++;
	return true;
}
#endif
#ifndef NAVIDROME_COORDINATOR_TEST
bool navidromeplay_album(const nd_library_t *library, const nd_query_t *query, int action, void *origin) {
	(void)library; (void)query; (void)action; (void)origin; return true;
}
bool navidromeplay_enqueuing(void) { return false; }
bool navidromeplay_enqueue(const nd_library_t *library, nd_kind_t kind, const char *id, int64_t session) {
    assert(library->ready && (kind==ND_TRACKS || kind==ND_ALBUMS));
    fixture_queue_adds++; queued_session=session;
    snprintf(queued_track,sizeof(queued_track),"%s",id?id:"");
    return true;
}
bool navidromeplay_matches(const nd_library_t *library, nd_kind_t kind, const char *id) {
    return library->card_epoch==marker_library.card_epoch && !strcmp(library->scope,marker_library.scope) &&
        !strcmp(id,kind==ND_TRACKS?selected_track:kind==ND_ALBUMS?selected_album:"");
}
#endif
#ifndef NAVIDROME_DOWNLOAD_UI
__attribute__((weak)) bool navidromedownloads_request(const nd_dl_request_t *request, nd_dl_captured_cb cb, void *user) {
    (void)request; (void)cb; (void)user; return false;
}
#endif
void back_btn_tint_on_screen(lv_obj_t *screen, bool on, lv_color_t color) { (void)screen; (void)on; (void)color; }
void topbar_tint_on_screen(lv_obj_t *screen, bool on, lv_color_t bg, lv_color_t ink) { (void)screen; (void)on; (void)bg; (void)ink; }
const char *tr(const char *key) {
#ifdef NAVIDROME_PLAYLIST_UI
	if (!strcmp(key, "playlist_tracks")) return "%d tracks";
	if (!strcmp(key, "playlist_track_2")) return "%d track";
	if (!strcmp(key, "nd_pl_no_tracks")) return "No tracks yet.";
#endif
	return key;
}
static unsigned fixture_notices;
void gui_notify_popup(const char *key) {
	(void)key;
	fixture_notices++;
}
#ifndef NAVIDROME_STAR_UI
static char fixture_last_success[80];
void toast_success(const char *key) { snprintf(fixture_last_success,sizeof(fixture_last_success),"%s",key); }
#endif
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
#ifndef NAVIDROME_PLAYLIST_UI
static bool fixture_album_art_ready;
static int fixture_album_art_size;
static unsigned fixture_album_art_requests;
void navidromeart_release(int slot) {
    if (slot == ND_ART_ALBUM_SLOT) fixture_album_art_size = 0;
}
void navidromeart_request(int slot, const nd_library_t *library, const char *id, int size) {
	if (slot == ND_ART_ALBUM_SLOT) { fixture_album_art_size = size; fixture_album_art_requests++; }
	(void)slot;
	(void)library;
	(void)id;
	(void)size;
}
bool navidromeart_take(int slot, cover_image_t *image, bool *finished) {
    if (slot == ND_ART_ALBUM_SLOT && fixture_album_art_size) {
        *finished = fixture_album_art_ready;
        if (!fixture_album_art_ready) return false;
        int size = fixture_album_art_size;
        memset(image,0,sizeof(*image));
        image->pixels = calloc((size_t)size*size,4); assert(image->pixels);
        for (int i=0;i<size*size;i++) { image->pixels[i*4+1]=255; image->pixels[i*4+3]=255; }
        image->dsc = (lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,.w=size,.h=size,.stride=size*4},.data=image->pixels,.data_size=(uint32_t)(size*size*4)};
        image->has_tone=true; image->tone=0x00ff00; image->tone_luma=149; image->tone_light=false;
        fixture_album_art_size=0;
        return true;
    }

	(void)slot;
	(void)image;
	*finished = true;
	return false;
}
#endif
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
#if defined(NAVIDROME_COORDINATOR_TEST) || defined(NAVIDROME_STAR_UI)
static atomic_bool fixture_reject_posts;
#endif
bool gui_post(void (*cb)(void *), void *data) {
#if defined(NAVIDROME_COORDINATOR_TEST) || defined(NAVIDROME_STAR_UI)
	if (atomic_load(&fixture_reject_posts))
		return false;
#endif
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
#ifndef NAVIDROME_BROWSE_FIXTURE_ONLY
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

#endif

static void seed(const nd_library_t *library) {
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s", library->path);
	*strrchr(dir, '/') = 0;
	for (char *p = dir + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		assert(!mkdir(dir, 0700) || errno == EEXIST);
		*p = '/';
	}
	assert(!mkdir(dir, 0700) || errno == EEXIST);
	nd_store_t store;
	assert(nd_store_open_source(&store, library->path, library->scope, true, false, library->favorites));
	assert(nd_store_begin(&store));
	assert(sqlite3_exec(store.db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_stmt *st;
	assert(sqlite3_prepare_v2(store.db, "INSERT INTO entity VALUES(?,?,?,?,'Fixture',?,'',120,?,?,?)", -1, &st, NULL) == SQLITE_OK);
	for (int kind = 0; kind < (library->favorites ? 2 : 4); kind++) {
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
	for (int i = 0; !library->favorites && i < 1700; i++) {
		char text[256];
		snprintf(text, sizeof(text), "INSERT INTO rel VALUES(%lld,'album-%05d',2,'artist-00000'); INSERT INTO rel VALUES(%lld,'album-%05d',3,'genre-00000');", (long long)store.generation, i, (long long)store.generation, i);
		assert(sqlite3_exec(store.db, text, NULL, NULL, NULL) == SQLITE_OK);
	}
	assert(sqlite3_exec(store.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
	if (library->favorites) {
		char text[512];
		snprintf(text, sizeof(text), "INSERT INTO favorite_album SELECT gen,id,10 FROM entity WHERE gen=%lld AND kind=0; INSERT INTO favorite_song SELECT gen,id,1 FROM entity WHERE gen=%lld AND kind=1", (long long)store.generation, (long long)store.generation);
		assert(sqlite3_exec(store.db, text, NULL, NULL, NULL) == SQLITE_OK);
	}
#ifdef NAVIDROME_POLISH_QUEUE_UI
	assert(sqlite3_exec(store.db,"UPDATE entity SET cover='queue-fixture' WHERE kind=1",NULL,NULL,NULL)==SQLITE_OK);
#endif
	assert(nd_store_validate(&store) && nd_store_publish(&store));
	nd_store_close(&store);
}

#if !defined(NAVIDROME_COORDINATOR_TEST) && !defined(NAVIDROME_PLAYLIST_UI) && !defined(NAVIDROME_STAR_UI) && !defined(NAVIDROME_BROWSE_FIXTURE_ONLY)
#include "navidrome_artistfilter_uifixture.h"

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
	font_ui_24 = font_ui_28 = font_ui_24_bold = font_ui_14 = font_ui_18 = font_ui_20 = font_ui_64_bold = lv_font_unscii_8;
	int width = getenv("SONIX_CHECK_WIDTH") ? atoi(getenv("SONIX_CHECK_WIDTH")) : 320;
	assert(width == 320 || width == 480);
	int height = width == 320 ? 480 : 800;
	ui_panel_width = width;
	lv_display_t *display = lv_display_create(width, height);
	navidrome_screen = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = width, .screen_height = height, .padding = 8, .top_bar_height = 20};
	navidromebrowse_init(&cfg);
	int requests_before = atomic_load(&calls);
	navidromebrowse_open(ND_TRACKS);
	window_wait(0);
	assert(pages[0].total == 17000 && pages[0].count == ND_WINDOW);
	assert(lv_obj_get_width(pages[0].images[0]) == ui_px(ND_ART_THUMB_SIZE));
	assert(lv_image_get_inner_align(pages[0].images[0]) == LV_IMAGE_ALIGN_CONTAIN);
	lv_area_t art_bounds, row_bounds, name_bounds;
	lv_obj_get_coords(pages[0].images[0], &art_bounds);
	lv_obj_get_coords(pages[0].rows[0], &row_bounds);
	lv_obj_get_coords(pages[0].names[0], &name_bounds);
	assert(art_bounds.y1 >= row_bounds.y1 && art_bounds.y2 <= row_bounds.y2);
	assert(art_bounds.x2 < name_bounds.x1);
	lv_obj_scroll_to_y(pages[0].list, 16995 * pitch, LV_ANIM_OFF);
	lv_obj_update_layout(pages[0].screen);
	lv_obj_send_event(pages[0].list, LV_EVENT_SCROLL, NULL);
	window_wait(0);
	assert(pages[0].first > 16980 && pages[0].first + pages[0].count == 17000);
	assert(!strcmp(pages[0].data[pages[0].count - 1].title, "Z Track 16999"));
	puts("real LVGL windowed scrolling reaches track 17000: passed");
	dragging = true;
	lv_obj_send_event(pages[0].rows[pages[0].count - 1], LV_EVENT_CLICKED, NULL);
	assert(!playback_selections);
	dragging = false;
	lv_obj_send_event(pages[0].rows[pages[0].count - 1], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == 1 && !strcmp(selected_track, pages[0].data[pages[0].count - 1].id));
	navidromebrowse_refresh();
	lv_obj_update_layout(pages[0].screen);
	for(int row=0;row<pages[0].count;row++) {
		lv_obj_t *mark = pages[0].playmarks[row];
		assert(lv_obj_is_hidden(mark) == !!strcmp(pages[0].data[row].id, selected_track));
		assert(lv_obj_get_width(mark) == ui_px(6) && lv_obj_get_height(mark) == ui_px(52));
		assert(lv_obj_get_style_radius(mark, 0) == LV_RADIUS_CIRCLE);
	}
	unsigned queued_before=fixture_queue_adds;
	lv_obj_send_event(pages[0].menus[0],LV_EVENT_CLICKED,NULL);
	bool found_queue=false;
	for(int item=0;item<fixture_menu_count;item++) if(!strcmp(fixture_menu[item].label,"add_to_queue")) {
		fixture_menu[item].action(fixture_menu[item].user); found_queue=true; break;
	}
	assert(found_queue && fixture_queue_adds==queued_before+1 && !queued_session && !strcmp(queued_track,pages[0].data[0].id));
	assert(playback_selections==1);
	puts("real LVGL exact-ID row Add to queue and selected-track accent without playback: passed");
	puts("real LVGL deliberate track selection and gesture guard: passed");
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
		lv_obj_send_event(pages[1].menus[0], LV_EVENT_CLICKED, NULL);
		bool has_playlist_action = false;
		for (int menu = 0; menu < fixture_menu_count; menu++)
			has_playlist_action |= !strcmp(fixture_menu[menu].label, "add_to_playlist");
		assert(has_playlist_action);
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
	puts("real LVGL four paths, parent scroll, gesture guards, no browsing-triggered network/playback: passed");
	unsigned before_selection = playback_selections;
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	for (int i = 0; i < 10000 && (navidrome_playlists_busy() || !pages[0].selecting); i++) {
		drain();
		usleep(1000);
	}
	window_wait(0);
	assert(pages[0].selecting && pages[0].selected == 1 && pages[0].chosen[0]);
	// A letter jump during an asynchronous toggle must not supersede its
	// completion and strand select_busy. The completion loads the latest scroll.
	lv_obj_send_event(pages[0].rows[1], LV_EVENT_CLICKED, NULL);
	assert(pages[0].select_busy);
	go(pages[0].index, 26);
	for (int i = 0; i < 10000 && (navidrome_playlists_busy() || pages[0].select_busy); i++) {
		drain();
		usleep(1000);
	}
	window_wait(0);
	assert(pages[0].selected == 2 && pages[0].first <= 25 * 654 && pages[0].first + pages[0].count > 25 * 654);
	lv_obj_scroll_to_y(pages[0].list, 500 * pitch, LV_ANIM_OFF);
	lv_obj_update_layout(pages[0].screen);
	lv_obj_send_event(pages[0].list, LV_EVENT_SCROLL, NULL);
	window_wait(0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	for (int i = 0; i < 10000 && (navidrome_playlists_busy() || pages[0].selected != 3); i++) {
		drain();
		usleep(1000);
	}
	window_wait(0);
	assert(pages[0].selected == 3 && pages[0].chosen[0]);
	assert(!lv_obj_is_hidden(pages[0].select_queue));
	queued_before=fixture_queue_adds;
	lv_obj_send_event(pages[0].select_queue,LV_EVENT_CLICKED,NULL);
	assert(fixture_queue_adds==queued_before+1 && queued_session==pages[0].session && !*queued_track);
	navidromebrowse_refresh();
	assert(!pages[0].select_busy && playback_selections==before_selection);
	assert(playback_selections == before_selection);
	lv_obj_send_event(pages[0].rows[1], LV_EVENT_CLICKED, NULL);
	assert(pages[0].select_busy);
	assert(browse_selection_back());
	for (int i = 0; i < 10000 && navidrome_playlists_busy(); i++) {
		drain();
		usleep(1000);
	}
	window_wait(0);
	assert(!pages[0].selecting && !pages[0].select_busy);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == before_selection + 1);
	puts("real LVGL long press, cross-window selection, concurrent letter jump/Cancel, and restored playback: passed");
	library = artist_ui_checks(library, dir);
	pages[0].picking=true;
	assert(navidromebrowse_artist("artist-00000","Duplicate label")); window_wait(0);
	assert(!pages[0].picking && pages[0].query.kind==ND_ALBUMS && !strcmp(pages[0].query.filter,"artist-00000") && pages[0].total==1700);
	assert(navidromebrowse_album("album-00000","Album")); window_wait(0);
	assert(!pages[0].picking && pages[0].query.kind==ND_TRACKS && pages[0].total==10);
	puts("direct artist/album entry on the already active browser screen reloads exact IDs and clears playlist picking: passed");

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
	snprintf(path, sizeof(path), "%s/.local/navidrome/.cache", dir);
	assert(!rmdir(path));
	snprintf(path, sizeof(path), "%s/.local/navidrome", dir);
	assert(!rmdir(path));
	snprintf(path, sizeof(path), "%s/.local", dir);
	assert(!rmdir(path));
	assert(!chdir("/") && !rmdir(dir));
	return 0;
}
#endif
