// Actual LVGL browse menus, selection store and service. Download admission is
// recorded here; downloadcheck separately exercises the actual workers/HTTP.
#define NAVIDROME_DOWNLOAD_UI 1
#define NAVIDROME_BROWSE_FIXTURE_ONLY 1
#include "navidrome_browsecheck.c"
static nd_dl_request_t download;
static unsigned download_submissions;
static nd_dl_captured_cb download_callback;
static void *download_user;
bool navidromedownloads_request(const nd_dl_request_t *q, nd_dl_captured_cb cb, void *user) {
	download = *q;
	download_submissions++;
	download_callback = cb;
	download_user = user;
	return true;
}
static void wait_window(int level) {
	for (int i = 0; i < 10000; i++) {
		drain();
		lv_obj_update_layout(pages[level].screen);
		if (!pages[level].loading && pages[level].count && !pages[level].select_busy && !navidrome_playlists_busy())
			return;
		usleep(1000);
	}
	assert(!"Download browser window timeout");
}
static void choose_download(void) {
	for (int i = 0; i < fixture_menu_count; i++)
		if (!strcmp(fixture_menu[i].label, "nd_dl_download")) {
			fixture_menu[i].action(fixture_menu[i].user);
			return;
		}
	assert(!"Download menu absent");
}
int main(void) {
	alarm(60);
	char dir[] = "/tmp/sonix-download-browse-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	wait_idle();
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	seed(&lib);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&lib);
		if (!lib.loading)
			break;
		usleep(1000);
	}
	assert(lib.ready);
	lv_init();
	font_ui_24 = font_ui_28 = font_ui_24_bold = font_ui_14 = font_ui_18 = font_ui_20 = font_ui_64_bold = lv_font_unscii_8;
	int width = getenv("SONIX_CHECK_WIDTH") ? atoi(getenv("SONIX_CHECK_WIDTH")) : 320;
	ui_panel_width = width;
	lv_display_create(width, width == 320 ? 480 : 800);
	navidrome_screen = lv_obj_create(NULL);
	gui_config_t config = {.screen_width = width, .screen_height = width == 320 ? 480 : 800, .padding = 8, .top_bar_height = 20};
	navidromebrowse_init(&config);
	navidromebrowse_open(ND_TRACKS);
	wait_window(0);
	browse_page_t *p = &pages[0];
	int calls_before = atomic_load(&calls);
	unsigned plays = playback_selections;
	lv_obj_send_event(p->menus[0], LV_EVENT_CLICKED, NULL);
	assert(!download_submissions && atomic_load(&calls) == calls_before);
	char exact[ND_ID_MAX];
	strcpy(exact, p->data[0].id);
	choose_download();
	assert(download_submissions == 1 && !download.session && download.kind == ND_TRACKS && !strcmp(download.id, exact));
	assert(p->select_busy && playback_selections == plays);
	download_callback(true, NULL, 1, 0, download_user);
	assert(!p->select_busy);
	lv_obj_send_event(p->menus[1], LV_EVENT_CLICKED, NULL);
	browse_menu_serial--;
	choose_download();
	assert(download_submissions == 1);
	lv_obj_send_event(p->rows[0], LV_EVENT_LONG_PRESSED, NULL);
	wait_window(0);
	assert(p->selecting && p->selected == 1);
	lv_obj_t *buttons[] = {p->select_download, p->select_queue, p->select_favorite, p->select_add, p->select_close};
	for (unsigned i = 0; i < 5; i++) {
		assert(!lv_obj_is_hidden(buttons[i]));
		lv_area_t a;
		lv_obj_get_coords(buttons[i], &a);
		assert(a.x1 >= ui_px(56) && a.x2 < width);
		for (unsigned k = 0; k < i; k++) {
			lv_area_t b;
			lv_obj_get_coords(buttons[k], &b);
			assert(a.x2 < b.x1 || a.x1 > b.x2);
		}
	}
	unsigned art = fixture_album_art_requests;
	lv_obj_send_event(p->select_download, LV_EVENT_CLICKED, NULL);
	assert(download_submissions == 2 && download.session == p->session && download.session && !download.id[0]);
	assert(playback_selections == plays && fixture_album_art_requests == art);
	assert(browse_selection_back());
	wait_window(0);
	download_callback(true, NULL, 1, 0, download_user);
	assert(!p->selecting && !p->select_busy);
	// Ordinary taps still play after selection Download and Cancel.
	lv_obj_send_event(p->rows[0], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == plays + 1);
	navidromebrowse_open(ND_ALBUMS);
	wait_window(0);
	p = &pages[0];
	lv_obj_send_event(p->menus[0], LV_EVENT_CLICKED, NULL);
	choose_download();
	assert(download.kind == ND_ALBUMS && !strcmp(download.id, p->data[0].id));
	download_callback(true, NULL, 10, 0, download_user);
	// The alternative album header submits the exact album, independently of
	// the visible track window and without replacing playback.
	lv_obj_send_event(p->rows[0], LV_EVENT_CLICKED, NULL);
	wait_window(1);
	p = &pages[1];
	assert(p->album.active && !lv_obj_is_hidden(p->album.buttons));
	lv_obj_update_layout(p->screen);
	lv_obj_t *album_download = p->album.controls[ND_ALBUM_DOWNLOAD];
	lv_area_t download_box;
	lv_obj_get_coords(album_download, &download_box);
	assert(download_box.x1 >= 0 && download_box.x2 < width);
	unsigned album_before = download_submissions;
	plays = playback_selections;
	lv_obj_send_event(album_download, LV_EVENT_CLICKED, NULL);
	assert(download_submissions == album_before + 1 && download.kind == ND_ALBUMS && !strcmp(download.id, p->query.filter) && !download.session);
	assert(playback_selections == plays && lv_obj_has_state(album_download, LV_STATE_DISABLED));
	download_callback(true, NULL, 10, 0, download_user);
	assert(!lv_obj_has_state(album_download, LV_STATE_DISABLED));
	p->selecting = true;
	browse_selection_ui(p);
	assert(lv_obj_is_hidden(p->album.buttons));
	p->selecting = false;
	p->picking = true;
	browse_selection_ui(p);
	assert(lv_obj_is_hidden(p->album.buttons));
	p->picking = false;
	browse_selection_ui(p);
	dragging = true;
	lv_obj_send_event(album_download, LV_EVENT_CLICKED, NULL);
	assert(download_submissions == album_before + 1);
	dragging = false;
	navidromebrowse_open(ND_ALBUMS);
	wait_window(0);
	p = &pages[0];
	// Download is suppressed when the same page is a playlist picker.
	p->picking = true;
	lv_obj_send_event(p->menus[0], LV_EVENT_CLICKED, NULL);
	for (int i = 0; i < fixture_menu_count; i++)
		assert(strcmp(fixture_menu[i].label, "nd_dl_download"));
	p->picking = false;
	lv_obj_send_event(p->menus[0], LV_EVENT_CLICKED, NULL);
	dragging = true;
	unsigned before = download_submissions;
	choose_download();
	assert(download_submissions == before);
	dragging = false;
	// The same album menus remain available through exact artist/genre paths.
	nd_kind_t roots[] = {ND_ARTISTS, ND_GENRES};
	for (unsigned kind = 0; kind < 2; kind++) {
		navidromebrowse_open(roots[kind]);
		wait_window(0);
		lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
		wait_window(1);
		p = &pages[1];
		lv_obj_send_event(p->menus[0], LV_EVENT_CLICKED, NULL);
		choose_download();
		assert(download.kind == ND_ALBUMS && !strcmp(download.id, p->data[0].id));
		download_callback(true, NULL, 10, 0, download_user);
	}
	nd_library_t favorites;
	navidrome_favorites_snapshot(&favorites);
	seed(&favorites);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_favorites_snapshot(&favorites);
		if (!favorites.loading)
			break;
		usleep(1000);
	}
	assert(favorites.ready);
	navidromebrowse_open_favorites(ND_TRACKS);
	wait_window(0);
	p = &pages[0];
	lv_obj_send_event(p->menus[0], LV_EVENT_CLICKED, NULL);
	choose_download();
	assert(download.library.favorites && download.kind == ND_TRACKS && !strcmp(download.id, p->data[0].id));
	download_callback(true, NULL, 1, 0, download_user);
	printf("PASS actual Download album-header shortcut/track/album menus, stable IDs, selection/Cancel races, five retained controls/artwork, picker exclusion and ordinary taps at %d pixels\n", width);
	return 0;
}
