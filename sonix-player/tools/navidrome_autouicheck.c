// Real LVGL display events and production page-family scheduler/service.
#define NAVIDROME_SETTINGS_MAIN unused_settings_main
#include "navidrome_uicheck.c"
#include "src/gui/streaming/navidromepage.c"

// Surrounding page refreshers are not exercised by the scheduler fixture.
void gridpage_set_dimmed(lv_obj_t *grid, int index, bool dimmed) { (void)grid; (void)index; (void)dimmed; }
void navidromebrowse_refresh(void) {}
void navidromeplaylists_refresh(void) {}
void navidromefavorites_update(void) {}
void navidromesearch_refresh(void) {}

int main(void) {
	alarm(40);
	char dir[] = "/tmp/sonix-auto-ui.XXXXXX";
	assert(mkdtemp(dir));
	snprintf(config_file, sizeof(config_file), "%s/config.ini", dir);
	navidrome_init();
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, NULL));
	assert(wait_idle().state == ND_CONNECTED);
	navidrome_test_online(0);
	navidrome_poll_network();
	navidrome_test_online(1);
	int before = atomic_load(&calls);
	lv_init();
	lv_display_t *display = lv_display_create(320, 480);
	lv_obj_t *outside = lv_obj_create(NULL), *root = lv_obj_create(NULL), *child = lv_obj_create(NULL);
	navidromepage_register(root);
	navidromepage_register(child);
	lv_screen_load(outside);
	entry_init();
	fixture_auto_preference = true;
	entry_tick(NULL);
	assert(atomic_load(&calls) == before); // Startup/local-only page.
	lv_screen_load(root);
	assert(wait_idle().state == ND_CONNECTED && atomic_load(&calls) == before + 1);
	lv_screen_load(child);
	lv_screen_load(root);
	entry_tick(NULL); // A sheet leaves this same underlying screen active.
	assert(atomic_load(&calls) == before + 1);
	navidrome_test_online(0);
	navidrome_poll_network();
	navidrome_test_online(1);
	lv_screen_load(child);
	lv_screen_load(root);
	assert(atomic_load(&calls) == before + 1); // No new episode inside the family.
	lv_screen_load(outside);
	lv_screen_load(child); // Direct relationship entry also belongs to Navidrome.
	assert(wait_idle().state == ND_CONNECTED && atomic_load(&calls) == before + 2);
	assert(navidrome_encrypt("vault1"));
	wait_idle();
	navidrome_lock();
	lv_screen_load(outside);
	lv_screen_load(root);
	assert(atomic_load(&calls) == before + 2 && wait_idle().state == ND_LOCKED);
	assert(navidrome_unlock("vault1"));
	assert(wait_idle().state == ND_CONNECTED && atomic_load(&calls) == before + 3);
	entry_tick(NULL);
	lv_screen_load(child);
	assert(atomic_load(&calls) == before + 3); // Unlock already supplied the check.
	lv_screen_load(outside);
	assert(lv_timer_get_paused(entry_timer));
	lv_timer_delete(entry_timer);
	entry_timer = NULL;
	navidrome_reset();
	lv_display_delete(display);
	lv_deinit();
	puts("real LVGL auto entry: outside/child/direct entry, sheet return, encrypted Unlock, no duplicates and paused outside scheduler: passed");
	return 0;
}
