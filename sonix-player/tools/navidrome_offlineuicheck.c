// Production LVGL browse/playlist/Search rows plus the real local status worker.
#include <fcntl.h>
#define NAVIDROME_OFFLINE_UI
#define keyboard_create fixture_unused_keyboard_create
#include "navidrome_playlistuicheck.c"
#undef keyboard_create
#include "src/system/decode/decode.h"
#include "src/system/streaming/navidromeoffline.c"

struct keyboard_s {
	lv_obj_t *tray;
	bool visible;
};
keyboard_t *keyboard_create(lv_obj_t *parent, int width, int height, lv_obj_t *field, const lv_image_dsc_t *icon, const char *text, lv_event_cb_t cb, void *user) {
	(void)field;
	(void)icon;
	(void)text;
	(void)cb;
	(void)user;
	keyboard_t *kb = calloc(1, sizeof(*kb));
	assert(kb);
	kb->tray = lv_obj_create(parent);
	lv_obj_set_size(kb->tray, width, height);
	lv_obj_align(kb->tray, LV_ALIGN_BOTTOM_MID, 0, 0);
	return kb;
}
void keyboard_set_visible(keyboard_t *kb, bool on) {
	kb->visible = on;
	lv_obj_set_hidden(kb->tray, !on);
}
bool keyboard_is_visible(keyboard_t *kb) { return kb->visible; }
const lv_image_dsc_t icon_search = FIXTURE_ICON, icon_clear = FIXTURE_ICON;
bool navidromeplay_search(const nd_library_t *lib, const nd_context_t *c, const nd_query_t *q, const char *id, void *origin) {
	(void)lib;
	(void)c;
	(void)q;
	(void)id;
	(void)origin;
	assert(!"status must not start search playback");
	return false;
}
#include "src/gui/streaming/navidromesearch.c"

decode_format_t decode_detect_format(const char *path) {
	(void)path;
	return DECODE_FORMAT_UNKNOWN;
}
decoder_t *decoder_open_info(const char *p, decode_format_t f) {
	(void)p;
	(void)f;
	return NULL;
}
int decoder_sample_rate(const decoder_t *d) {
	(void)d;
	return 0;
}
int decoder_channels(const decoder_t *d) {
	(void)d;
	return 0;
}
void decoder_close(decoder_t *d) { (void)d; }

static void badge_wait(nd_off_badges_t *v) {
	for (int i = 0; i < 20000; i++) {
		drain();
		navidromeofflinebadge_tick(v);
		lv_obj_update_layout(v->screen);
		if (!nd_offline_busy(v->token)) {
			drain();
			navidromeofflinebadge_tick(v);
			return;
		}
		usleep(1000);
	}
	assert(!"offline GUI timeout");
}
static void badge_pixels(nd_off_badges_t *v, int row, nd_off_state_t expected) {
	assert(v->state[row] == expected);
	lv_obj_t *icon = v->icons[row];
	bool shown = expected == ND_OFF_COMPLETE || expected == ND_OFF_PARTIAL;
	assert(lv_obj_is_hidden(icon) != shown);
	if (!shown)
		return;
	assert(lv_image_get_src(icon) == &icon_download && icon_download.header.w > 1);
	assert(!lv_obj_is_clickable(icon));
	lv_area_t image, detail;
	lv_obj_get_coords(icon, &image);
	lv_obj_get_coords(v->details[row], &detail);
	assert(image.x1 > detail.x2 && image.x2 < (int)ui_panel_width);
	assert(lv_obj_get_width(icon) == ui_px(20));
	lv_draw_buf_t *pixels = lv_snapshot_take(icon, LV_COLOR_FORMAT_ARGB8888);
	assert(pixels);
	unsigned painted = 0;
	for (unsigned y = 0; y < pixels->header.h; y++)
		for (unsigned x = 0; x < pixels->header.w; x++) {
			const unsigned char *p = pixels->data + y * pixels->header.stride + x * 4;
			if (p[3] > 128) {
				if (expected == ND_OFF_COMPLETE)
					assert(p[1] > p[0] && p[1] > p[2]);
				else
					assert(abs((int)p[0] - p[1]) < 3 && abs((int)p[1] - p[2]) < 3);
				painted++;
			}
		}
	assert(painted >= 6);
	lv_draw_buf_destroy(pixels);
}
static void seed_original(nd_library_t *lib, const char *wav, int index) {
	nd_dl_store_t store;
	assert(nd_dl_open(&store, lib, true));
	assert(nd_dl_sql(&store, "INSERT OR IGNORE INTO batch(id,destination,created) VALUES(1,'Music/Navidrome',0)"));
	nd_playrow_t row = {0};
	snprintf(row.row.id, sizeof(row.row.id), "song-%05d", index);
	int added = 0, skipped = 0;
	assert(nd_dl_insert(&store, 1, "Music/Navidrome", &row, 0, &added, &skipped));
	nd_dl_job_t job;
	assert(nd_dl_next(&store, &job));
	int dir = nd_dl_directory(store.root, "Music/Navidrome/Album", true);
	assert(dir >= 0);
	char leaf[40];
	snprintf(leaf, sizeof(leaf), "%d.wav", index);
	int in = open(wav, O_RDONLY), out = openat(dir, leaf, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(in >= 0 && out >= 0);
	char bytes[4096];
	ssize_t n;
	while ((n = read(in, bytes, sizeof(bytes))) > 0)
		assert(write(out, bytes, n) == n);
	assert(!n && !close(in) && !close(out));
	struct stat st;
	assert(nd_dl_fingerprint(dir, leaf, job.digest, &st, NULL, NULL));
	close(dir);
	job.state = NDDL_COMPLETE;
	job.bytes = st.st_size;
	job.mtime = st.st_mtime;
	job.inode = st.st_ino;
	snprintf(job.path, sizeof(job.path), "Music/Navidrome/Album/%s", leaf);
	strcpy(job.extension, ".wav");
	assert(nd_dl_update(&store, &job));
	nd_dl_close(&store);
	nd_dl_availability_changed();
}
int main(int argc, char **argv) {
	assert(argc == 3 || argc == 4);
	alarm(90);
	int height = argc == 4 ? atoi(argv[3]) : atoi(argv[1]) == 320 ? 480 : 800;
	int width = atoi(argv[1]);
	ui_panel_width = width;
	char dir[] = "/tmp/sonix-offline-ui.XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	wait_idle();
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	seed(&lib);
	navidrome_set_root(dir);
	library_wait(&lib);
	lv_init();
	font_ui_28 = font_ui_24_bold = font_ui_14 = font_ui_16 = font_ui_18 = font_ui_20 = font_ui_24 = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(width, height);
	navidrome_screen = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = width, .screen_height = height, .padding = 8, .top_bar_height = 20};
	navidromebrowse_init(&cfg);
	navidromeplaylists_init(&cfg);
	navidromesearch_init(&cfg);
	int requests = atomic_load(&calls);
	seed_original(&lib, argv[2], 0);
	navidromebrowse_open(ND_ALBUMS);
	window_wait(0);
	badge_wait(&pages[0].offline);
	badge_pixels(&pages[0].offline, 0, ND_OFF_PARTIAL);
	for (int i = 1; i < 10; i++)
		seed_original(&lib, argv[2], i);
	navidromeofflinebadge_tick(&pages[0].offline);
	badge_wait(&pages[0].offline);
	badge_pixels(&pages[0].offline, 0, ND_OFF_COMPLETE);
	assert(navidromebrowse_album("album-00000", "Album"));
	window_wait(0);
	badge_wait(&pages[0].offline);
	assert(pages[0].album.active);
	badge_pixels(&pages[0].offline, 0, ND_OFF_COMPLETE);
	lv_obj_set_style_bg_color(pages[0].rows[0], lv_color_hex(0x151515), 0);
	badge_paint(&pages[0].offline, 0);
	badge_pixels(&pages[0].offline, 0, ND_OFF_COMPLETE);
	lv_obj_set_style_bg_color(pages[0].rows[0], lv_color_hex(0xf5f5f5), 0);
	badge_paint(&pages[0].offline, 0);
	badge_pixels(&pages[0].offline, 0, ND_OFF_COMPLETE);
	lv_obj_add_state(pages[0].rows[0], LV_STATE_PRESSED);
	badge_pixels(&pages[0].offline, 0, ND_OFF_COMPLETE);
	lv_obj_remove_state(pages[0].rows[0], LV_STATE_PRESSED);
	navidromealbum_set_enabled(false);
	navidromebrowse_refresh();
	window_wait(0);
	badge_wait(&pages[0].offline);
	badge_pixels(&pages[0].offline, 0, ND_OFF_COMPLETE);
	unsigned retired_token = pages[0].offline.token;
	submit_window(&pages[0]);
	window_wait(0);
	badge_wait(&pages[0].offline);
	badge_done(retired_token, 0, ND_OFF_PARTIAL, &pages[0].offline);
	badge_pixels(&pages[0].offline, 0, ND_OFF_COMPLETE);
	puts("PASS real native glyph colors/pixels, Library/album rows, numbered/cover layouts, theme/selection and retired row delivery");

	nd_playlist_store_t saved;
	char path[1200];
	assert(nd_pl_path(&lib, path) && nd_pl_open(&saved, path, lib.scope, true));
	for (int p = 0; p < 2; p++) {
		int64_t id;
		assert(nd_pl_create(&saved, p ? "Partial" : "Complete", &id) && nd_pl_begin(&saved, id, 1));
		int added = 0, skipped = 0;
		for (int i = 0; i < 3; i++) {
			nd_row_t row = {0};
			snprintf(row.id, sizeof(row.id), "song-%05d", p && i == 1 ? 20 : i % 2);
			strcpy(row.title, "Long translated title for downloaded track");
			strcpy(row.artist, "Long translated artist credit");
			assert(nd_pl_append(&saved, id, &row, false, &added, &skipped));
		}
		assert(nd_pl_finish(&saved, id, true) && nd_pl_sql(&saved, "COMMIT"));
	}
	nd_pl_close(&saved);
	navidromeplaylists_open();
	pl_wait(0);
	badge_wait(&pl_pages[0].offline);
	badge_pixels(&pl_pages[0].offline, 0, ND_OFF_COMPLETE);
	badge_pixels(&pl_pages[0].offline, 1, ND_OFF_PARTIAL);
	pl_open_detail(&pl_pages[0].playlists[0]);
	pl_wait(1);
	badge_wait(&pl_pages[1].offline);
	for (int i = 0; i < 3; i++)
		badge_pixels(&pl_pages[1].offline, i, ND_OFF_COMPLETE);
	assert(!download_submissions && !playback_selections);
	puts("PASS saved playlist aggregate and repeated detail entries, secondary-label geometry and unchanged download/playback controls");

	navidromesearch_open();
	search_page_t *p = &search_pages[0];
	p->kind = ND_TRACKS;
	p->data.library = lib;
	p->data.source = ND_SEARCH_SERVER;
	p->data.count = p->data.total = 1;
	strcpy(p->data.rows[0].id, "song-00000");
	strcpy(p->data.rows[0].title, "Downloaded Search result");
	p->populated = true;
	search_paint_rows(p);
	badge_wait(&p->offline);
	badge_pixels(&p->offline, 0, ND_OFF_COMPLETE);
	assert(keyboard_is_visible(search_keyboard));
	keyboard_set_visible(search_keyboard, false);
	search_layout(p);
	badge_pixels(&p->offline, 0, ND_OFF_COMPLETE);
	p->kind = ND_ALBUMS;
	strcpy(p->data.rows[0].id, "unsynced-summary");
	search_paint_rows(p);
	badge_wait(&p->offline);
	badge_pixels(&p->offline, 0, ND_OFF_UNKNOWN);
	assert(atomic_load(&calls) == requests);
	navidrome_lock();
	navidromesearch_refresh();
	drain();
	assert(lv_screen_active() == navidrome_screen);
	assert(!p->offline.bound && !download_submissions);
	puts("PASS Search exact tracks, unknown summaries, keyboard layouts and Lock; zero indicator HTTP or playback");
	navidrome_shutdown();
	drain();
	lv_display_delete(display);
	lv_deinit();
	return 0;
}
