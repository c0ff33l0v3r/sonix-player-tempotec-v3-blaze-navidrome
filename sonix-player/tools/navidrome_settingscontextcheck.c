// Exercise the production context policy with real LVGL controls. No audio device.
#include "src/gui/settings/musicsettings.c"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int32_t ui_panel_width = 320;
lv_font_t font_ui_14;
extern const lv_font_t lv_font_unscii_8;
static device_state_t fixture_state;
void device_state_get(device_state_t *state) { *state = fixture_state; }
bool nd_audio_source(const char *path) { return path && (!strncmp(path, "ndq:", 4) || strstr(path, "/.local/navidrome/.cache/audio/")); }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
int main(int argc, char **argv) {
	ui_panel_width = argc > 1 ? atoi(argv[1]) : 320;
	font_ui_14 = lv_font_unscii_8;
	lv_init();
	lv_display_t *display = lv_display_create(ui_panel_width, ui_panel_width == 320 ? 480 : 800);
	musicsettings_screen = lv_obj_create(NULL);
	lv_obj_t *child = lv_obj_create(NULL);
	lv_obj_t *scan = local_setting(lv_obj_create(musicsettings_screen));
	lv_obj_t *library = local_setting(lv_obj_create(musicsettings_screen));
	lv_obj_t *folder = local_setting(lv_obj_create(child));
	lv_obj_t *saved = local_setting(lv_switch_create(child));
	lv_obj_add_state(saved, LV_STATE_CHECKED);
	lv_obj_t *eq = lv_obj_create(musicsettings_screen);
	lv_obj_t *queue = lv_obj_create(child);
	lv_obj_t *tap = lv_obj_create(child);
	snprintf(fixture_state.current_file, sizeof(fixture_state.current_file), "ndq:1:12");
	fixture_state.status = AUDIO_STATUS_STOPPED;
	musicsettings_open(true);
	assert(lv_obj_is_hidden(scan) && lv_obj_is_hidden(library));
	lv_screen_load(child);
	context_refresh(NULL);
	assert(lv_obj_is_hidden(folder) && lv_obj_is_hidden(saved));
	assert(lv_obj_has_state(saved, LV_STATE_CHECKED));
	assert(!lv_obj_is_hidden(eq) && !lv_obj_is_hidden(queue) && !lv_obj_is_hidden(tap));
	snprintf(fixture_state.current_file, sizeof(fixture_state.current_file), "/card/.local/navidrome/.cache/audio/scope/file.flac");
	fixture_state.status = AUDIO_STATUS_PAUSED;
	context_refresh(NULL);
	assert(lv_obj_is_hidden(scan));
	musicsettings_open(false);
	assert(!lv_obj_is_hidden(scan) && !lv_obj_is_hidden(folder));
	assert(lv_obj_has_state(saved, LV_STATE_CHECKED));
	musicsettings_open(true);
	snprintf(fixture_state.current_file, sizeof(fixture_state.current_file), "/card/local.flac");
	context_refresh(NULL);
	assert(!lv_obj_is_hidden(scan) && !lv_obj_is_hidden(saved));
	lv_display_delete(display);
	lv_deinit();
	printf("real LVGL settings context at %d px: player/global entry, child controls, paused/cached/unavailable selection, source change and hidden switch retention passed\n", ui_panel_width);
	return 0;
}
