#define NAVIDROME_BROWSE_FIXTURE_ONLY 1
#include "navidrome_browsecheck.c"
#undef NAVIDROME_BROWSE_FIXTURE_ONLY
#include "navidrome_album_store_fixture.h"

static void album_cover_rendered(lv_obj_t *image) {
	lv_obj_update_layout(image);
	lv_draw_buf_t *snapshot = lv_snapshot_take(image, LV_COLOR_FORMAT_ARGB8888);
	assert(snapshot);
	int green = 0;
	for (unsigned y = 0; y < snapshot->header.h; y++)
		for (unsigned x = 0; x < snapshot->header.w; x++) {
			const unsigned char *px = snapshot->data + y * snapshot->header.stride + x * 4;
			green += px[1] > 200 && px[0] < 30 && px[2] < 30 && px[3] > 200;
		}
	int side = lv_obj_get_width(image);
	assert(green >= side * side * 95 / 100);
	lv_draw_buf_destroy(snapshot);
}
static void album_wait(void) {
	for (int i = 0; i < 10000; i++) {
		drain();
		lv_obj_update_layout(pages[0].screen);
		if (!pages[0].loading && pages[0].count)
			return;
		usleep(1000);
	}
	assert(!"album window timeout");
}
static void album_scroll(int y) {
	lv_obj_scroll_to_y(pages[0].list, y, LV_ANIM_OFF);
	submit_window(&pages[0]);
	album_wait();
}
int main(void) {
	alarm(60);
	char dir[] = "/tmp/sonix-album-ui-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	album_store_check();
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	wait_idle();
	nd_library_t library;
	navidrome_library_snapshot(&library);
	seed(&library);
	// A 17,000-track album with 170 discs exercises virtual headings without
	// a proportional GUI allocation or the reference Music page's 3,000 cap.
	nd_store_t store;
	assert(nd_store_open(&store, library.path, library.scope, true, false));
	assert(sqlite3_exec(store.db, "UPDATE entity SET album='album-00000',disc=(CAST(substr(id,6) AS INT)/100)+1,track=(CAST(substr(id,6) AS INT)%100)+1 WHERE kind=1", NULL, NULL, NULL) == SQLITE_OK);
	assert(sqlite3_exec(store.db, "UPDATE entity SET cover='header-cover' WHERE kind=0 AND id='album-00000'", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&store);
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
	ui_panel_width = width;
	lv_display_t *display = lv_display_create(width, width == 320 ? 480 : 800);
	(void)display;
	navidrome_screen = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = width, .screen_height = width == 320 ? 480 : 800, .padding = 8, .top_bar_height = 20};
	navidromebrowse_init(&cfg);
	int before = atomic_load(&calls);
	assert(navidromebrowse_album("album-00000", "Fixture album"));
	album_wait();
	browse_page_t *p = &pages[0];
	assert(p->album.active && p->total == 17000 && p->count == ND_WINDOW);
	assert(p->album_window.heading[0] && !*p->data[0].id);
	assert(!lv_obj_is_clickable(p->rows[0]));
	assert(lv_obj_get_height(p->rows[0]) == ui_px(44));
	assert(!strcmp(lv_label_get_text(p->numbers[1]), "1"));
	assert(lv_obj_is_hidden(p->images[1]));
	assert(lv_image_get_inner_align(p->album.cover) == LV_IMAGE_ALIGN_CONTAIN_DOWNSCALE);
	assert(lv_image_get_scale_x(p->album.cover) == LV_SCALE_NONE);
	assert(!*lv_label_get_text(p->title));
	fixture_album_art_ready = true;
	navidromebrowse_refresh();
	assert(p->album.image.pixels && p->album.tinted);
	assert(lv_image_get_inner_align(p->album.cover) == LV_IMAGE_ALIGN_CONTAIN);
	album_cover_rendered(p->album.cover);
	unsigned requested = fixture_album_art_requests;
	submit_window(p);
	album_wait();
	assert(fixture_album_art_requests == requested);
	// A displayed offline fallback is not permanently settled across Reconnect.
	// Keep it visible while its replacement is pending; do not move the list.
	uint8_t *previous_pixels = p->album.image.pixels;
	int previous_scroll = lv_obj_get_scroll_y(p->list);
	navidrome_test_online(0);
	navidrome_poll_network();
	navidromebrowse_refresh();
	fixture_album_art_ready = false;
	navidrome_test_online(1);
	assert(navidrome_reconnect());
	wait_idle();
	navidromebrowse_refresh();
	assert(fixture_album_art_requests == requested + 1);
	assert(p->album.image.pixels == previous_pixels && !p->album.art_done);
	fixture_album_art_ready = true;
	navidromebrowse_refresh();
	assert(p->album.art_done && p->album.image.pixels && p->album.image.pixels != previous_pixels);
	assert(lv_obj_get_scroll_y(p->list) == previous_scroll);
	assert(atomic_load(&calls) == before + 1 && !playback_selections);
	before = atomic_load(&calls); // Only the explicitly requested Reconnect ping.
	p->data[1].track = 0;
	navidromealbum_row(&p->album, p->rows[1], p->names[1], p->details[1], p->numbers[1], p->images[1], &p->data[1], false);
	assert(!strcmp(lv_label_get_text(p->numbers[1]), "—"));

	album_scroll(p->album.height + 16000 * pitch + 160 * ui_px(44));
	assert(p->count <= ND_WINDOW && p->album_window.ordinal[p->count - 1] > 15990);
	assert(*lv_label_get_text(p->title));
	// Pin the visible exact track while changing presentation behind settings.
	char anchor[ND_ID_MAX] = "";
	int scroll = lv_obj_get_scroll_y(p->list);
	for (int i = 0; i < p->count; i++)
		if (*p->data[i].id && lv_obj_get_y(p->rows[i]) + pitch > scroll) {
			memcpy(anchor, p->data[i].id, sizeof(anchor));
			break;
		}
	assert(*anchor);
	navidromealbum_set_enabled(false);
	assert(lv_image_get_inner_align(p->album.cover) == LV_IMAGE_ALIGN_CONTAIN_DOWNSCALE);
	assert(lv_image_get_scale_x(p->album.cover) == LV_SCALE_NONE);
	navidromebrowse_refresh();
	album_wait();
	assert(!p->album.active && !p->album.height && !p->album.tinted);
	bool found = false;
	for (int i = 0; i < p->count; i++)
		found |= !strcmp(anchor, p->data[i].id);
	assert(found && !lv_obj_is_hidden(p->images[0]) && lv_obj_is_hidden(p->numbers[0]));
	navidromealbum_set_enabled(true);
	navidromebrowse_refresh();
	album_wait();
	assert(p->album.active);
	found = false;
	for (int i = 0; i < p->count; i++)
		found |= !strcmp(anchor, p->data[i].id);
	assert(found);
	// Picking and selection suppress header playback without changing IDs.
	p->picking = true;
	submit_window(p);
	album_wait();
	assert(lv_obj_is_hidden(p->album.buttons));
	p->picking = false;
	p->selecting = true;
	browse_selection_ui(p);
	assert(lv_obj_is_hidden(p->album.buttons));
	assert(!lv_obj_is_hidden(p->selection_status));
	assert(lv_obj_get_parent(p->selection_status) == p->screen);
	p->selecting = false;
	browse_selection_ui(p);
	assert(!lv_obj_is_hidden(p->album.buttons));
	assert(lv_obj_is_hidden(p->selection_status));
	// The real color policy runs on a supplied deterministic worker tone.
	p->album.image.has_tone = true;
	p->album.image.tone = 0xffff00;
	p->album.image.tone_luma = 210;
	p->album.image.tone_light = true;
	album_tone(&p->album);
	assert(p->album.tinted);
	int luma = (p->album.bg.red * 299 + p->album.bg.green * 587 + p->album.bg.blue * 114) / 1000;
	assert(luma >= COVER_PAGE_LIGHT_FROM && p->album.ink.red < 40);
	assert(navidromebrowse_album("album-00000", "Fixture album"));
	album_wait();
	assert(atomic_load(&calls) == before && playback_selections == 0);
	printf("album LVGL %dx%d: 17,000 tracks/170 discs, numbering, title follow, both layouts/anchoring, picking and tint: passed\n", width, width == 320 ? 480 : 800);
	navidrome_shutdown();
}
