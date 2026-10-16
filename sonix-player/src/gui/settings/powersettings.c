#include "powersettings.h"

#include <stdio.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/axpcharge.h"
#include "src/system/device/led.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/device/sysinfo.h"
#include "src/gui/shell/switcher.h"

lv_obj_t *powersettings_screen;

// 80 to 100 in fives. Stopping short of full is what keeps a lithium cell
// healthy if the device mostly lives on the charger.
#define CHARGE_LIMIT_MIN 80
#define CHARGE_LIMIT_STEP 5
#define CHARGE_LIMIT_COUNT 5 // 80, 85, 90, 95, 100

static const struct {
	int minutes;
	const char *label;
} AUTO_OFF[] = {
	{5, "power_5_minutes"}, {10, "power_10_minutes"}, {15, "power_15_minutes"}, {30, "power_30_minutes"}, {60, "power_1_hour"}, {120, "power_2_hours"}, {180, "power_3_hours"}, {360, "power_6_hours"},
};
#define AUTO_OFF_COUNT ((int)(sizeof(AUTO_OFF) / sizeof(AUTO_OFF[0])))

static lv_obj_t *charge_value, *charge_slider;
static lv_obj_t *auto_off_switch, *auto_off_value, *auto_off_slider, *auto_off_card;
static lv_obj_t *led_on_switch, *led_off_switch, *led_off_card;
static lv_obj_t *key_lock_switch;
static lv_obj_t *standby_switch;
static lv_obj_t *charge_note;
static lv_obj_t *axp_limit_switch, *axp_current_switch; // the R1's two, instead of the slider

// --- LED colours ---
//
// One row per state; a tap moves it to the next colour and shows it on the LED
// for a moment. The patterns are the ones leds_sgm31324_add.sh registers on
// the Blaze (12-14 added there by hand); 9 and 10 are left out, they look the
// same as 14 and 13.
static const struct {
	int pattern;
	const char *label;
} LED_COLOURS[] = {
	{5, "led_colour_off"},		   {1, "led_colour_blue"},	 {3, "led_colour_cyan"},
	{14, "led_colour_dark_blue"},  {8, "led_colour_green"},	 {12, "led_colour_green_blink"},
	{2, "led_colour_yellow"},	   {4, "led_colour_orange"}, {6, "led_colour_red_breathing"},
	{7, "led_colour_red_blink"},   {13, "led_colour_purple"}, {11, "led_colour_white"},
};
#define LED_COLOUR_COUNT ((int)(sizeof(LED_COLOURS) / sizeof(LED_COLOURS[0])))
#define LED_PREVIEW_MS 3000

static const char *const LED_STATE_LABELS[LED_STATE_COUNT] = {
	[LED_STATE_IDLE] = "led_state_idle",		   [LED_STATE_PCM_48K] = "led_state_pcm_48k",
	[LED_STATE_PCM_192K] = "led_state_pcm_192k",   [LED_STATE_PCM_384K] = "led_state_pcm_384k",
	[LED_STATE_DSD] = "led_state_dsd",			   [LED_STATE_PODCAST] = "led_state_podcast",
	[LED_STATE_CHARGING] = "led_state_charging",   [LED_STATE_WIFI] = "led_state_wifi",
	[LED_STATE_BT_RX] = "led_state_bt_receiver",	[LED_STATE_PCM_96K] = "led_state_pcm_96k",
	[LED_STATE_PCM_768K] = "led_state_pcm_768k",
};

// The rows in page order: the enum is in saved-index order, with the PCM ranges
// added later at its end.
static const led_state_t LED_STATE_ORDER[LED_STATE_COUNT] = {
	LED_STATE_IDLE,		  LED_STATE_PCM_48K, LED_STATE_PCM_96K,	 LED_STATE_PCM_192K,
	LED_STATE_PCM_384K,	  LED_STATE_PCM_768K, LED_STATE_DSD,	 LED_STATE_PODCAST,
	LED_STATE_CHARGING,	  LED_STATE_WIFI,	 LED_STATE_BT_RX,
};

static lv_obj_t *led_colours_screen;
static lv_obj_t *led_colour_values[LED_STATE_COUNT];
static lv_timer_t *led_preview_timer;

static int led_colour_index(int pattern) {
	for (int i = 0; i < LED_COLOUR_COUNT; i++) {
		if (LED_COLOURS[i].pattern == pattern) {
			return i;
		}
	}
	return -1;
}

static void led_colour_label(led_state_t state) {
	int i = led_colour_index(led_state_pattern(state));
	lv_label_set_text(led_colour_values[state], i >= 0 ? tr(LED_COLOURS[i].label) : "?");
}

static void led_preview_done_cb(lv_timer_t *t) {
	(void)t;
	led_preview_timer = NULL;
	led_preview_end();
}

static void led_colour_row_cb(lv_event_t *e) {
	led_state_t state = (led_state_t)(intptr_t)lv_event_get_user_data(e);
	int next = LED_COLOURS[(led_colour_index(led_state_pattern(state)) + 1) % LED_COLOUR_COUNT].pattern;
	led_set_state_pattern(state, next);
	char key[24];
	snprintf(key, sizeof(key), "colour_%d", (int)state);
	config_set_int("led", key, next);
	led_colour_label(state);

	led_preview(next);
	if (led_preview_timer) {
		lv_timer_reset(led_preview_timer);
	} else {
		led_preview_timer = lv_timer_create(led_preview_done_cb, LED_PREVIEW_MS, NULL);
		lv_timer_set_repeat_count(led_preview_timer, 1);
	}
}

static void build_led_colours_page(gui_config_t *cfg) {
	led_colours_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(led_colours_screen, cfg, "power_led_colours");
	for (int i = 0; i < LED_STATE_COUNT; i++) {
		led_state_t s = LED_STATE_ORDER[i];
		settingsrow_add(container, LED_STATE_LABELS[s], &led_colour_values[s], led_colour_row_cb, (void *)(intptr_t)s);
		led_colour_label(s);
	}
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("power_led_colours_note"));
	switcher_attach_back_gesture(led_colours_screen);
}

// --- reading the saved values ---

static int charge_index(void) {
	int percent = (int)config_get_int("power", "charge_limit", 100);
	int index = (percent - CHARGE_LIMIT_MIN) / CHARGE_LIMIT_STEP;
	if (index < 0) {
		index = 0;
	}
	if (index >= CHARGE_LIMIT_COUNT) {
		index = CHARGE_LIMIT_COUNT - 1;
	}
	return index;
}

static int auto_off_index(void) {
	int minutes = (int)config_get_int("power", "auto_off_minutes", 30);
	for (int i = 0; i < AUTO_OFF_COUNT; i++) {
		if (AUTO_OFF[i].minutes == minutes) {
			return i;
		}
	}
	return 3; // the 30 minute default
}

// --- applying them ---

static void set_checked(lv_obj_t *sw, bool on) {
	if (on) {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(sw, LV_STATE_CHECKED);
	}
}

void powersettings_apply(void) {
	if (axpcharge_applies()) {
		power_set_charge_limit(100);
		axpcharge_set(config_get_bool("power", "charge_limit_80", false),
					  config_get_bool("power", "charge_500ma", false));
	} else {
		power_set_charge_limit(CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));
	}

	power_set_auto_off(config_get_bool("power", "auto_off", false), (uint32_t)AUTO_OFF[auto_off_index()].minutes);

	// Suspend-to-RAM standby, on by default: saves battery by suspending the
	// player shortly after the screen goes off.
	power_set_standby_enabled(config_get_bool("power", "standby_mem", true));

	// The LED master switch comes first: with it off there is nothing for the
	// standby option to turn off, and led.c keeps the charge indicator alive
	// on its own regardless.
	led_set_enabled(config_get_bool("power", "led_on", true));
	led_set_idle_off(config_get_bool("power", "led_off_standby", true));
	for (int s = 0; s < LED_STATE_COUNT; s++) {
		char key[24];
		snprintf(key, sizeof(key), "colour_%d", s);
		// A range split off later, never set, follows the colour saved for the
		// one it used to be part of (88.2-96 was "up to 192", 768 "above 192").
		int fallback = s == LED_STATE_PCM_96K	 ? led_state_pattern(LED_STATE_PCM_192K)
					   : s == LED_STATE_PCM_768K ? led_state_pattern(LED_STATE_PCM_384K)
												 : 0;
		int pattern = (int)config_get_int("led", key, fallback);
		if (led_colour_index(pattern) >= 0) {
			led_set_state_pattern((led_state_t)s, pattern);
		}
	}

	input_set_key_lock(config_get_bool("power", "lock_keys_screen_off", false));
}

static void refresh_labels(void) {
	if (led_colours_screen) {
		for (int s = 0; s < LED_STATE_COUNT; s++) {
			led_colour_label((led_state_t)s);
		}
	}
	if (charge_value) {
		lv_label_set_text_fmt(charge_value, "%d%%", CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));
	}
	if (axp_limit_switch) {
		set_checked(axp_limit_switch, config_get_bool("power", "charge_limit_80", false));
		set_checked(axp_current_switch, config_get_bool("power", "charge_500ma", false));
	}
	lv_label_set_text(auto_off_value, tr(AUTO_OFF[auto_off_index()].label));

	// The LED master switch, and the standby option that hangs off it: with the
	// LED switched off entirely there is nothing left for the standby option to
	// do, so it greys out.
	bool led_on = config_get_bool("power", "led_on", true);
	if (led_on) {
		lv_obj_add_state(led_on_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(led_on_switch, LV_STATE_CHECKED);
	}

	if (config_get_bool("power", "led_off_standby", true)) {
		lv_obj_add_state(led_off_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(led_off_switch, LV_STATE_CHECKED);
	}

	if (led_on) {
		lv_obj_remove_state(led_off_switch, LV_STATE_DISABLED);
		lv_obj_set_style_opa(led_off_card, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_state(led_off_switch, LV_STATE_DISABLED);
		lv_obj_set_style_opa(led_off_card, LV_OPA_40, 0);
	}

	set_checked(key_lock_switch, config_get_bool("power", "lock_keys_screen_off", false));

	if (config_get_bool("power", "standby_mem", true)) {
		lv_obj_add_state(standby_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(standby_switch, LV_STATE_CHECKED);
	}

	// One card: the slider is only on it while the toggle is on.
	bool auto_off = config_get_bool("power", "auto_off", false);
	if (auto_off) {
		lv_obj_add_state(auto_off_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(auto_off_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(auto_off_card, auto_off);
}

static void charge_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(charge_slider);
	config_set_int("power", "charge_limit", CHARGE_LIMIT_MIN + (index * CHARGE_LIMIT_STEP));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void axp_limit_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "charge_limit_80", lv_obj_has_state(axp_limit_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void axp_current_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "charge_500ma", lv_obj_has_state(axp_current_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(auto_off_slider);
	config_set_int("power", "auto_off_minutes", AUTO_OFF[index].minutes);
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void led_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_off_standby", lv_obj_has_state(led_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

// The master switch. With the LED off the standby option has nothing left to
// do, so it greys out -- and the charger still lights the LED regardless, so
// a charge in progress is never invisible.
static void led_on_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_on", lv_obj_has_state(led_on_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void key_lock_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "lock_keys_screen_off", lv_obj_has_state(key_lock_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "auto_off", lv_obj_has_state(auto_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void standby_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "standby_mem", lv_obj_has_state(standby_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

void powersettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(powersettings_screen, cfg, "power");

	// Suspend-to-RAM standby: suspends the player shortly after the screen goes
	// off to save battery, and playback resumes on wake. On by default. What it
	// waits for is the screen going dark, and that timeout lives on the Screen
	// page, beside the brightness it belongs with.
	settingsrow_toggle(container, "power_standby_mem", &standby_switch, standby_toggled_cb);
	lv_obj_t *standby_note = lv_label_create(container);
	lv_label_set_long_mode(standby_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(standby_note, lv_pct(100));
	lv_obj_add_style(standby_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(standby_note, &font_ui_22, 0);
	lv_label_set_text(standby_note, tr("power_standby_mem_note"));

	// The R1 charges through its PMIC, which takes a lower target voltage and a
	// lower current rather than a percentage: two switches there, the slider
	// everywhere else.
	bool pmic = axpcharge_applies();
	if (pmic) {
		settingsrow_toggle(container, "power_charge_limit_80", &axp_limit_switch, axp_limit_toggled_cb);
		lv_obj_t *limit_note = lv_label_create(container);
		lv_label_set_long_mode(limit_note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(limit_note, lv_pct(100));
		lv_obj_add_style(limit_note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(limit_note, &font_ui_22, 0);
		lv_label_set_text(limit_note, tr("power_charge_limit_80_note"));
		settingsrow_toggle(container, "power_charge_500ma", &axp_current_switch, axp_current_toggled_cb);
	} else {
		settingsrow_slider(container, "power_charge_limit", CHARGE_LIMIT_COUNT, &charge_value, &charge_slider,
						   charge_changed_cb);
		lv_slider_set_value(charge_slider, charge_index(), LV_ANIM_OFF);
	}

	// The LED master switch, and the standby option that depends on it.
	settingsrow_toggle(container, "power_led_on", &led_on_switch, led_on_toggled_cb);
	led_off_card = settingsrow_toggle(container, "power_led_off_in_standby", &led_off_switch, led_off_toggled_cb);
	// Only where the palette above is what the LED module registers.
	if (sysinfo_model()->ak4493) {
		build_led_colours_page(cfg);
		settingsrow_add(container, "power_led_colours", NULL, switch_screen_cb, led_colours_screen);
	}
	// No "turn Wi-Fi off in standby" entry: parking the radio is automatic, and
	// wifi_in_use() in power.c knows on its own when not to turn it off --
	// Qobuz, Tidal, podcasts, radio, AirPlay, DLNA, transfers.

	// The lock gesture for the buttons on the case (system.c).
	settingsrow_toggle(container, "power_lock_keys_screen_off", &key_lock_switch, key_lock_toggled_cb);
	lv_obj_t *key_lock_note = lv_label_create(container);
	lv_label_set_long_mode(key_lock_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(key_lock_note, lv_pct(100));
	lv_obj_add_style(key_lock_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(key_lock_note, &font_ui_22, 0);
	lv_label_set_text(key_lock_note, tr("power_lock_keys_screen_off_note"));

	// Toggle and "after how long" on one card: the slider is only there while
	// the option is on.
	auto_off_card = settingsrow_toggle_slider(container, "power_auto_power_off", AUTO_OFF_COUNT,
											  &auto_off_switch, &auto_off_value, &auto_off_slider,
											  auto_off_toggled_cb, auto_off_changed_cb);
	lv_slider_set_value(auto_off_slider, auto_off_index(), LV_ANIM_OFF);

	// If the charger driver has no control node there is nothing to enforce
	// the limit with, and saying so is better than a setting that silently
	// does nothing.
	charge_note = lv_label_create(container);
	lv_label_set_long_mode(charge_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(charge_note, lv_pct(100));
	lv_obj_add_style(charge_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(charge_note, &font_ui_22, 0);
	lv_label_set_text(charge_note, pmic || power_charge_limit_supported() ? tr("power_auto_off_note")
																	   : tr("power_charge_limit_unavailable"));

	// Everything saved becomes effective now, at startup -- including the LED
	// standby option, which lives in led.c rather than power.c.
	powersettings_apply();
	refresh_labels();
}
