#include "navidromesettings.h"
#include "navidromealbum.h"
#include "navidromedownloadpage.h"
#include "navidromepage.h"
#include "src/gui/nowplaying/player.h"
#include "src/system/device/power.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/navidrome.h"
#include "src/system/streaming/navidromecache.h"
#include "src/system/streaming/navidromeplay.h"
#include "src/system/streaming/navidromedebug.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef struct {
	lv_obj_t *screen, *body, *status;
	lv_obj_t *fields[3], *eyes[3];
	keyboard_t *keyboard;
} form_t;
typedef enum { PASSPHRASE_CREATE, PASSPHRASE_UNLOCK, PASSPHRASE_CHANGE, PASSPHRASE_ENCRYPT, PASSPHRASE_PLAIN } pass_mode_t;
static form_t main_form, pass_form, settings_form;
static lv_obj_t *encrypt_switch, *encrypt_row, *protection_note, *pass_return;
static bool encrypt_selected;
static lv_obj_t *debug_switch, *debug_note, *album_view_switch, *auto_connect_switch;
static gui_config_t *config;
static lv_obj_t *save_button, *update_button, *reconnect_button, *unlock_button;
static lv_obj_t *lock_button, *forget_button, *change_button, *pass_button;
static bool updating, pass_waiting;
static lv_obj_t *sync_button, *sync_cancel_button, *library_status, *sync_status;
static lv_obj_t *cache_clear, *cache_total, *cache_values[2], *cache_bars[2];
static bool cache_usage_needed;
static unsigned cache_notice;
static bool cache_was_clearing;
static unsigned clear_card_epoch, clear_profile_epoch;
static pass_mode_t pass_mode;
static unsigned loaded_revision;
static struct {
	char host[ND_HOST_MAX], user[ND_USER_MAX], password[ND_SECRET_MAX];
} staged;

static bool gesture(void) { return switcher_back_drag_active(); }
static void actions(void);
static void save_cb(lv_event_t *e);
static void pass_accept_cb(lv_event_t *e);
static void sync_actions(void);
static void cache_refresh(void);

static void wipe_field(lv_obj_t *field) {
	// LVGL owns this mutable storage (including pwd_tmp in password mode).
	// Clear it before asking LVGL to release/reallocate the old text.
	const char *text = lv_textarea_get_text(field);
	nd_wipe((char *)text, strlen(text));
	lv_textarea_set_text(field, "");
}

static void keyboard_visible(form_t *f, bool visible) {
	keyboard_set_visible(f->keyboard, visible);
	int top = settingsrow_content_top(config);
	lv_obj_set_height(f->body, config->screen_height - top - config->padding - (visible ? ui_px(316) : 0));
}

static void focus_cb(lv_event_t *e) {
	if (gesture())
		return;
	form_t *f = lv_event_get_user_data(e);
	lv_obj_t *field = lv_event_get_target(e);
	keyboard_visible(f, true);
	keyboard_set_field(f->keyboard, field);
	for (int i = 0; i < 3; i++) {
		bool active = f->fields[i] == field;
		if (active)
			lv_obj_add_state(f->fields[i], LV_STATE_FOCUSED);
		else
			lv_obj_remove_state(f->fields[i], LV_STATE_FOCUSED);
		keyboard_show_caret(f->fields[i], active);
	}
	lv_obj_scroll_to_view(field, LV_ANIM_ON);
}

static void eye_cb(lv_event_t *e) {
	if (gesture())
		return;
	lv_obj_t *field = lv_event_get_user_data(e);
	bool hidden = !lv_textarea_get_password_mode(field);
	lv_textarea_set_password_mode(field, hidden);
	keyboard_refresh_password(field);
	lv_image_set_src(lv_obj_get_child(lv_event_get_target(e), 0), hidden ? &icon_eye : &icon_eye_off);
}

static void field_changed_cb(lv_event_t *e) {
	if (updating)
		return;
	(void)e;
	actions();
}

static lv_obj_t *action(form_t *f, const char *key, lv_event_cb_t cb) {
	lv_obj_t *row = settingsrow_action(f->body, key, cb, NULL);
	lv_obj_set_style_opa(row, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_set_event_bubble(row, true);
	return row;
}

static void enable(lv_obj_t *obj, bool enabled) {
	// LVGL suppresses pointer events on disabled hit targets, including their
	// bubbled navigation events. Let the enclosing row/body receive that press.
	lv_obj_set_clickable(obj, enabled);
	if (enabled)
		lv_obj_remove_state(obj, LV_STATE_DISABLED);
	else
		lv_obj_add_state(obj, LV_STATE_DISABLED);
}

static void visible(lv_obj_t *obj, bool show) { lv_obj_set_hidden(obj, !show); }

static void make_field(form_t *f, int i, const char *key, bool secret, unsigned max_chars) {
	lv_obj_t *row = lv_obj_create(f->body);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, ui_px(88));
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), ui_px(88));
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr(key));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_t *field = f->fields[i] = lv_textarea_create(row);
	lv_textarea_set_one_line(field, true);
	// Cursor updates still scroll text programmatically. Touch drags belong
	// to page scrolling/back navigation rather than the one-line text viewport.
	lv_obj_set_scrollable(field, false);
	lv_obj_set_event_bubble(field, true);
	lv_textarea_set_max_length(field, max_chars);
	lv_obj_set_size(field, lv_pct(100), ui_px(58));
	lv_obj_align(field, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_text_font(field, &font_ui_20, 0);
	lv_obj_set_style_pad_all(field, ui_px(10), 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	keyboard_style_caret(field);
	keyboard_show_caret(field, false);
	lv_obj_add_event_cb(field, focus_cb, LV_EVENT_CLICKED, f);
	if (f == &main_form)
		lv_obj_add_event_cb(field, field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	if (secret) {
		keyboard_style_password(field, 0);
		lv_obj_set_style_pad_right(field, ui_px(56), 0);
		lv_obj_t *eye = f->eyes[i] = lv_btn_create(row);
		lv_obj_set_size(eye, ui_px(54), ui_px(54));
		lv_obj_align(eye, LV_ALIGN_BOTTOM_RIGHT, 0, -ui_px(2));
		lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
		lv_obj_set_style_shadow_width(eye, 0, 0);
		lv_obj_set_style_border_width(eye, 0, 0);
		lv_obj_set_event_bubble(eye, true);
		lv_obj_add_event_cb(eye, eye_cb, LV_EVENT_CLICKED, field);
		lv_obj_t *icon = lv_image_create(eye);
		lv_image_set_src(icon, &icon_eye);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_center(icon);
	}
}

static void form_init(form_t *f, const char *title) {
	f->screen = lv_obj_create(NULL);
	navidromepage_register(f->screen);
	lv_obj_add_style(f->screen, &theme_style_screen, 0);
	f->body = settingsrow_page(f->screen, config, title);
	settingsrow_title_corner_slots(settingsrow_page_title(f->screen), config, 0);
	f->status = lv_label_create(f->body);
	lv_obj_set_width(f->status, lv_pct(100));
	lv_obj_add_style(f->status, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(f->status, &font_ui_18, 0);
	lv_label_set_text(f->status, "");
	switcher_attach_back_gesture(f->screen);
	switcher_attach_back_gesture(f->body);
}

static bool identical(const navidrome_snapshot_t *s) { return !strcmp(lv_textarea_get_text(main_form.fields[0]), s->host) && !strcmp(lv_textarea_get_text(main_form.fields[1]), s->user); }

static void actions(void) {
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	bool editable = (!s.configured || s.unlocked) && !s.busy;
	for (int i = 0; i < 3; i++)
		enable(main_form.fields[i], editable);
	enable(main_form.eyes[2], editable);
	bool same = identical(&s);
	char saved[ND_SECRET_MAX];
	navidrome_password(saved);
	const char *text = lv_textarea_get_text(main_form.fields[2]);
	bool changed_password = strcmp(text, saved) != 0;
	nd_wipe(saved, sizeof(saved));
	bool save = editable && (!s.configured || !s.authenticated || !same);
	visible(save_button, !s.configured || (s.unlocked && (!s.authenticated || !same)));
	enable(save_button, save);
	visible(update_button, s.unlocked && s.authenticated && same);
	enable(update_button, editable && same && *text && changed_password);
	visible(reconnect_button, s.configured && s.unlocked);
	enable(reconnect_button, !s.busy);
	visible(unlock_button, s.configured && s.encrypted && !s.unlocked && !s.invalid);
	enable(unlock_button, !s.busy);
	visible(lock_button, s.encrypted && (s.unlocked || s.busy));
	visible(change_button, s.encrypted && s.unlocked);
	enable(change_button, !s.busy);
	visible(forget_button, s.configured || s.busy);
	lv_label_set_text(main_form.status, tr(s.busy ? (s.state == ND_CONNECTING ? "nd_connecting" : "nd_working") : s.error ? s.error : navidrome_feedback(&s)));
	if (!editable)
		keyboard_visible(&main_form, false);
	lv_label_set_text(settings_form.status, lv_label_get_text(main_form.status));
	bool encrypted = s.configured ? s.encrypted : encrypt_selected;
	if (encrypted)
		lv_obj_add_state(encrypt_switch, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(encrypt_switch, LV_STATE_CHECKED);
	enable(encrypt_row, !s.busy && !s.invalid);
	enable(encrypt_switch, !s.busy && !s.invalid);
	lv_label_set_text(protection_note, tr(encrypted ? "nd_encryption_on_note" : "nd_encryption_off_note"));
	lv_obj_set_state(album_view_switch, LV_STATE_CHECKED, navidromealbum_enabled());
	lv_obj_set_state(auto_connect_switch, LV_STATE_CHECKED, config_get_bool("navidrome", "auto_connect", false));
	nd_debug_state_t debug;
	nd_debug_snapshot(&debug);
	if (debug.enabled)
		lv_obj_add_state(debug_switch, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(debug_switch, LV_STATE_CHECKED);
	lv_label_set_text(debug_note, debug.enabled ? debug.pending ? tr("nd_working") : debug.recording ? debug.path : tr(debug.error ? debug.error : "nd_debug_storage") : tr("nd_debug_off"));
	sync_actions();
}

static void load_main(void) {
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	updating = true;
	for (int i = 0; i < 3; i++) {
		wipe_field(main_form.fields[i]);
		keyboard_show_caret(main_form.fields[i], false);
		lv_obj_remove_state(main_form.fields[i], LV_STATE_FOCUSED);
	}
	lv_textarea_set_text(main_form.fields[0], s.host);
	lv_textarea_set_text(main_form.fields[1], s.user);
	char password[ND_SECRET_MAX];
	if (navidrome_password(password))
		lv_textarea_set_text(main_form.fields[2], password);
	nd_wipe(password, sizeof(password));
	lv_textarea_set_password_mode(main_form.fields[2], true);
	keyboard_refresh_password(main_form.fields[2]);
	lv_image_set_src(lv_obj_get_child(main_form.eyes[2], 0), &icon_eye);
	loaded_revision = s.revision;
	updating = false;
	keyboard_reset(main_form.keyboard);
	keyboard_visible(&main_form, false);
	lv_obj_scroll_to_y(main_form.body, 0, LV_ANIM_OFF);
	actions();
}

static void main_loaded_cb(lv_event_t *e) {
	(void)e;
	load_main();
}

static void main_unloaded_cb(lv_event_t *e) {
	(void)e;
	updating = true;
	for (int i = 0; i < 3; i++)
		wipe_field(main_form.fields[i]);
	updating = false;
	keyboard_visible(&main_form, false);
}

static void pass_open(pass_mode_t mode) {
	pass_mode = mode;
	pass_waiting = false;
	for (int i = 0; i < 3; i++) {
		wipe_field(pass_form.fields[i]);
		keyboard_show_caret(pass_form.fields[i], false);
		lv_obj_remove_state(pass_form.fields[i], LV_STATE_FOCUSED);
		lv_textarea_set_password_mode(pass_form.fields[i], true);
		keyboard_refresh_password(pass_form.fields[i]);
		lv_image_set_src(lv_obj_get_child(pass_form.eyes[i], 0), &icon_eye);
		enable(pass_form.fields[i], true);
		enable(pass_form.eyes[i], true);
	}
	// Field 0 is current/unlock, field 1 new, field 2 confirmation.
	visible(lv_obj_get_parent(pass_form.fields[0]), mode != PASSPHRASE_CREATE && mode != PASSPHRASE_ENCRYPT);
	visible(lv_obj_get_parent(pass_form.fields[1]), mode != PASSPHRASE_UNLOCK && mode != PASSPHRASE_PLAIN);
	visible(lv_obj_get_parent(pass_form.fields[2]), mode != PASSPHRASE_UNLOCK && mode != PASSPHRASE_PLAIN);
	lv_label_set_text(pass_form.status, tr((mode == PASSPHRASE_CREATE || mode == PASSPHRASE_ENCRYPT) ? "nd_passphrase_note" : mode == PASSPHRASE_PLAIN ? "nd_disable_encryption_note" : mode == PASSPHRASE_UNLOCK ? "nd_unlock_note" : "nd_change_note"));
	enable(pass_button, true);
	keyboard_reset(pass_form.keyboard);
	keyboard_visible(&pass_form, false);
	lv_obj_scroll_to_y(pass_form.body, 0, LV_ANIM_OFF);
	pass_return = lv_screen_active();
	switch_screen(pass_form.screen);
}

static void save_cb(lv_event_t *e) {
	(void)e;
	if (gesture())
		return;
	const char *host = lv_textarea_get_text(main_form.fields[0]);
	const char *user = lv_textarea_get_text(main_form.fields[1]);
	const char *password = lv_textarea_get_text(main_form.fields[2]);
	char normalized[ND_HOST_MAX];
	const char *error = navidrome_validate(host, user, password, normalized);
	if (error) {
		gui_notify_popup(error);
		return;
	}
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	if (!s.configured && encrypt_selected) {
		nd_wipe(&staged, sizeof(staged));
		lv_snprintf(staged.host, sizeof(staged.host), "%s", normalized);
		lv_snprintf(staged.user, sizeof(staged.user), "%s", user);
		lv_snprintf(staged.password, sizeof(staged.password), "%s", password);
		pass_open(PASSPHRASE_CREATE);
	} else {
		if (s.authenticated && identical(&s)) {
			// Keyboard OK follows the same action as the visible password button.
			if (lv_obj_has_state(update_button, LV_STATE_DISABLED)) return;
			navidrome_update_password(host, user, password);
		}
		else
			navidrome_save(host, user, password, NULL);
		actions();
	}
}

static void reconnect_cb(lv_event_t *e) {
	(void)e;
	if (!gesture()) {
		navidrome_reconnect();
		actions();
	}
}
static void unlock_cb(lv_event_t *e) {
	(void)e;
	if (!gesture())
		pass_open(PASSPHRASE_UNLOCK);
}
static void change_cb(lv_event_t *e) {
	(void)e;
	if (!gesture())
		pass_open(PASSPHRASE_CHANGE);
}
static void lock_cb(lv_event_t *e) {
	(void)e;
	if (!gesture()) {
		navidrome_lock();
		load_main();
	}
}
static void forget_confirmed(void *unused) {
	(void)unused;
	encrypt_selected = false;
	navidrome_forget();
	load_main();
}
static void forget_cb(lv_event_t *e) {
	(void)e;
	if (!gesture())
		confirm_show(tr("nd_forget"), tr("nd_forget_confirm"), tr("nd_forget"), forget_confirmed, NULL);
}

static void pass_accept_cb(lv_event_t *e) {
	(void)e;
	if (gesture() || pass_waiting)
		return;
	const char *current = lv_textarea_get_text(pass_form.fields[0]);
	const char *replacement = lv_textarea_get_text(pass_form.fields[1]);
	const char *confirmation = lv_textarea_get_text(pass_form.fields[2]);
	if (pass_mode != PASSPHRASE_UNLOCK && pass_mode != PASSPHRASE_PLAIN) {
		if (strlen(replacement) >= ND_SECRET_MAX || strlen(current) >= ND_SECRET_MAX) {
			gui_notify_popup("nd_input_long");
			return;
		}
		if (!nd_passphrase_valid(replacement)) {
			gui_notify_popup("nd_passphrase_short");
			return;
		}
		if (strcmp(replacement, confirmation)) {
			gui_notify_popup("nd_passphrase_mismatch");
			return;
		}
	}
	bool ok = pass_mode == PASSPHRASE_CREATE ? navidrome_save(staged.host, staged.user, staged.password, replacement) : pass_mode == PASSPHRASE_UNLOCK ? navidrome_unlock(current) : pass_mode == PASSPHRASE_ENCRYPT ? navidrome_encrypt(replacement) : pass_mode == PASSPHRASE_PLAIN ? navidrome_unencrypt(current) : navidrome_change_passphrase(current, replacement);
	if (!ok) {
		navidrome_snapshot_t s;
		navidrome_snapshot(&s);
		gui_notify_popup(s.error ? s.error : "nd_worker_error");
		return;
	}
	pass_waiting = true;
	for (int i = 0; i < 3; i++) {
		wipe_field(pass_form.fields[i]);
		enable(pass_form.fields[i], false);
		enable(pass_form.eyes[i], false);
	}
	keyboard_visible(&pass_form, false);
	enable(pass_button, false);
	lv_label_set_text(pass_form.status, tr("nd_working"));
	// Retain initial credentials for retry until commit succeeds or this page
	// is left. The worker owns its copy; unlock password inputs are cleared above.
}

static void pass_unloaded_cb(lv_event_t *e) {
	(void)e;
	pass_waiting = false;
	for (int i = 0; i < 3; i++)
		wipe_field(pass_form.fields[i]);
	nd_wipe(&staged, sizeof(staged));
	keyboard_visible(&pass_form, false);
}

static void cache_clear_confirmed(void *unused) {
	(void)unused;
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	navidrome_snapshot_t snapshot;
	navidrome_snapshot(&snapshot);
	if (!snapshot.unlocked) { gui_notify_popup("nd_unlock_required"); return; }
	nd_context_t identity = {0};
	bool same = navidrome_context(&identity,false) && identity.card_epoch==clear_card_epoch && identity.profile_epoch==clear_profile_epoch;
	nd_wipe(&identity,sizeof(identity));
	if (!same) { gui_notify_popup("nd_cache_changed"); return; }
	if (nd_cache_clear_request(&lib, navidromeplay_cache_reader_active)) {
		navidromeplay_cache_stop();
		cache_was_clearing = true;
		cache_refresh();
	} else gui_notify_popup(lib.writable ? "nd_cache_busy" : "nd_cache_readonly");
}
static void cache_clear_cb(lv_event_t *event) {
	(void)event;
	if (gesture()) return;
	nd_context_t identity = {0};
	bool available = navidrome_context(&identity,false);
	clear_card_epoch=identity.card_epoch;clear_profile_epoch=identity.profile_epoch;
	nd_wipe(&identity,sizeof(identity));
	if (!available) { gui_notify_popup("nd_unlock_required"); return; }
	confirm_show("nd_cache_clear", "nd_cache_clear_confirm", "nd_cache_clear", cache_clear_confirmed, NULL);
}
static lv_timer_t *refresh_timer;
static void cache_refresh(void) {
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	nd_cache_usage_t usage;
	nd_cache_usage_snapshot(&usage);
	if (refresh_timer) {
		navidrome_snapshot_t activity;
		navidrome_snapshot(&activity);
		if (activity.busy || usage.busy)
			lv_timer_resume(refresh_timer);
		else
			lv_timer_pause(refresh_timer);
	}
	if (cache_was_clearing && !usage.busy && usage.revision != cache_notice) {
		cache_notice = usage.revision;
		cache_was_clearing = false;
		if (usage.card_epoch == lib.card_epoch && lv_screen_active() == settings_form.screen)
			gui_notify_popup(usage.error ? usage.error : "nd_cache_cleared");
	}
	if (!cache_total || lv_screen_active() != settings_form.screen) return;
	navidrome_snapshot_t snapshot;
	navidrome_snapshot(&snapshot);
	// Periodic byte counting must not change the action's enabled appearance.
	enable(cache_clear, snapshot.unlocked && lib.writable && *lib.path && !snapshot.busy && !usage.clearing);
	bool ready = usage.ready && usage.card_epoch == lib.card_epoch;
	char text[128];
	if (!snapshot.unlocked) lv_label_set_text(cache_total, tr("nd_unlock_required"));
	else if (!*lib.path && !lib.loading) lv_label_set_text(cache_total, tr("nd_cache_storage"));
	else if (usage.clearing) lv_label_set_text(cache_total, tr("nd_cache_clearing"));
	else if (usage.error && usage.card_epoch == lib.card_epoch) lv_label_set_text(cache_total, tr(usage.error));
	else if (ready) {
		uint64_t total = usage.audio + usage.art;
		lv_snprintf(text, sizeof(text), tr("nd_cache_total"), (unsigned long long)(total / (1024 * 1024)), (unsigned long long)(total % (1024 * 1024) * 100 / (1024 * 1024)));
		lv_label_set_text(cache_total, text);
	} else lv_label_set_text(cache_total, tr(snapshot.unlocked ? "nd_working" : "nd_unlock_required"));
	const uint64_t bytes[] = {usage.audio, usage.art}, budgets[] = {ND_AUDIO_BUDGET, ND_ART_BUDGET};
	const char *names[] = {"nd_cache_audio", "nd_cache_artwork"};
	for (int i = 0; i < 2; i++) {
		lv_snprintf(text, sizeof(text), "%s: %llu / %llu MiB", tr(names[i]), (unsigned long long)(ready ? bytes[i] / (1024 * 1024) : 0), (unsigned long long)(budgets[i] / (1024 * 1024)));
		lv_label_set_text(cache_values[i], text);
		lv_bar_set_value(cache_bars[i], ready ? (int)(bytes[i] >= budgets[i] ? 100 : bytes[i] * 100 / budgets[i]) : 0, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(cache_bars[i], theme()->accent, LV_PART_INDICATOR);
	}
	if (!usage.busy && snapshot.unlocked && *lib.path && cache_usage_needed) {
		if (nd_cache_usage_request(&lib)) {
			cache_usage_needed = false;
			if (refresh_timer)
				lv_timer_resume(refresh_timer);
		}
	}
}
static void cache_card_build(void) {
	lv_obj_t *card = lv_obj_create(settings_form.body);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ui_px(12), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, ui_px(20), 0);
	lv_obj_set_style_pad_ver(card, ui_px(16), 0);
	lv_obj_set_style_pad_row(card, ui_px(12), 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr("nd_cache_size"));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);
	for (int i = 0; i < 2; i++) {
		cache_values[i] = lv_label_create(card);
		lv_obj_set_width(cache_values[i], lv_pct(100));
		lv_label_set_long_mode(cache_values[i], LV_LABEL_LONG_WRAP);
		lv_obj_add_style(cache_values[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(cache_values[i], &font_ui_20, 0);
		cache_bars[i] = lv_bar_create(card);
		lv_obj_set_clickable(cache_bars[i], false);
		lv_obj_set_event_bubble(cache_bars[i], true);
		lv_obj_set_width(cache_bars[i], lv_pct(100));
		lv_obj_set_height(cache_bars[i], ui_px(10));
		lv_bar_set_range(cache_bars[i], 0, 100);
		lv_obj_set_style_radius(cache_bars[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_bg_color(cache_bars[i], theme()->text_secondary, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(cache_bars[i], LV_OPA_40, LV_PART_MAIN);
		lv_obj_set_style_radius(cache_bars[i], LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(cache_bars[i], theme()->accent, LV_PART_INDICATOR);
	}
	cache_total = lv_label_create(card);
	lv_obj_set_width(cache_total, lv_pct(100));
	lv_label_set_long_mode(cache_total, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(cache_total, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(cache_total, &font_ui_18, 0);
	cache_clear = action(&settings_form, "nd_cache_clear", cache_clear_cb);
}

void navidromesettings_refresh(void) {
	if (!main_form.screen)
		return;
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	if (pass_waiting && pass_mode == PASSPHRASE_CREATE && s.configured)
		nd_wipe(&staged, sizeof(staged)); // Committed, even if the network check is still pending.
	if (lv_screen_active() == settings_form.screen)
		actions();
	if (lv_screen_active() == main_form.screen) {
		if (s.revision != loaded_revision)
			load_main();
		else
			actions();
	}
	if (lv_screen_active() == pass_form.screen && pass_waiting && !s.busy) {
		pass_waiting = false;
		bool success = pass_mode == PASSPHRASE_CREATE ? s.configured && s.unlocked : pass_mode == PASSPHRASE_UNLOCK ? s.unlocked : pass_mode == PASSPHRASE_ENCRYPT ? s.encrypted && s.unlocked : pass_mode == PASSPHRASE_PLAIN ? !s.encrypted && s.unlocked : !s.error;
		if (success)
			switch_screen_return_to(pass_return);
		else {
			lv_label_set_text(pass_form.status, tr(s.error ? s.error : "nd_unlock_required"));
			for (int i = 0; i < 3; i++) {
				enable(pass_form.fields[i], true);
				enable(pass_form.eyes[i], true);
			}
			enable(pass_button, true);
		}
	}
	if (lv_screen_active() == pass_form.screen && pass_waiting && s.busy)
		lv_label_set_text(pass_form.status, tr(s.state == ND_CONNECTING ? "nd_connecting" : "nd_working"));
	cache_refresh();
}

void navidromesettings_open(void) { switch_screen(settings_form.screen); }

static void refresh_tick(lv_timer_t *timer) {
	(void)timer;
	cache_refresh();
	// A full GUI post queue may drop a completion notice. Reading the snapshot
	// here is local and also keeps an active unlock password page from staying busy.
	if (lv_screen_active() == main_form.screen || lv_screen_active() == settings_form.screen || lv_screen_active() == pass_form.screen)
		navidromesettings_refresh();
}

static const char *sync_reason(const navidrome_snapshot_t *s, const nd_library_t *library) {
	if (!s->unlocked)
		return "nd_unlock_required";
	if (s->busy)
		return "nd_working";
	if (s->state != ND_CONNECTED)
		return "nd_reconnect_required";
	if (!library->writable || !library->scope[0] || !library->path[0])
		return library->error ? library->error : "nd_cache_storage";
	return NULL;
}

static void sync_actions(void) {
	if (!library_status)
		return;
	navidrome_snapshot_t s;
	nd_library_t library;
	navidrome_snapshot(&s);
	navidrome_library_snapshot(&library);
	char text[512], stamp[80];
	struct tm time_info;
	if (library.ready && localtime_r(&library.finished, &time_info) && strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &time_info))
		lv_snprintf(text, sizeof(text), tr("nd_sync_summary"), stamp, library.counts[ND_TRACKS], library.counts[ND_ALBUMS], library.counts[ND_ARTISTS], library.counts[ND_GENRES]);
	else
		lv_snprintf(text, sizeof(text), "%s", tr("nd_not_synced"));
	lv_label_set_text(library_status, text);
	const char *reason = sync_reason(&s, &library);
	visible(sync_button, !library.syncing);
	enable(sync_button, !reason);
	visible(sync_cancel_button, library.syncing);
	if (library.syncing) {
		static const char *const stages[] = {"albums", "navidrome_tracks", "artists", "music_genres"};
		lv_snprintf(text, sizeof(text), tr("nd_sync_progress"), tr(stages[library.stage >= 0 && library.stage < 4 ? library.stage : 0]), library.fetched);
		lv_label_set_text(sync_status, text);
	} else
		lv_label_set_text(sync_status, tr(library.error ? library.error : reason ? reason : "nd_sync_ready"));
}

static void sync_cb(lv_event_t *e) {
	(void)e;
	if (gesture())
		return;
	navidrome_snapshot_t s;
	nd_library_t library;
	navidrome_snapshot(&s);
	navidrome_library_snapshot(&library);
	const char *reason = sync_reason(&s, &library);
	if (reason)
		gui_notify_popup(reason);
	else if (!navidrome_sync())
		gui_notify_popup("nd_sync_unavailable");
	actions();
}

static void sync_cancel_cb(lv_event_t *e) {
	(void)e;
	if (!gesture()) {
		navidrome_sync_cancel();
		actions();
	}
}

static void server_cb(lv_event_t *e) {
	(void)e;
	if (!gesture())
		switch_screen(main_form.screen);
}

static void encrypt_cb(lv_event_t *e) {
	(void)e;
	if (gesture()) {
		actions();
		return;
	}
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	bool wanted = lv_obj_has_state(encrypt_switch, LV_STATE_CHECKED);
	if (!s.configured)
		encrypt_selected = wanted;
	else if (!s.busy && !s.invalid && wanted != s.encrypted) {
		if (wanted)
			pass_open(PASSPHRASE_ENCRYPT);
		else if (s.unlocked)
			navidrome_unencrypt(NULL);
		else
			pass_open(PASSPHRASE_PLAIN);
	}
	actions();
}

static void settings_loaded_cb(lv_event_t *e) {
	cache_usage_needed = true;
	(void)e;
	actions();
	cache_refresh();
}

static void debug_cb(lv_event_t *e) {
	(void)e;
	if (!gesture()) {
		bool enabled = lv_obj_has_state(debug_switch, LV_STATE_CHECKED);
		config_set_bool("navidrome", "debug", enabled);
		config_save();
		nd_debug_enable(enabled);
	}
	actions();
}

static void album_view_cb(lv_event_t *e) {
	(void)e;
	if (!gesture() && !player_sheet_drag_active())
		navidromealbum_set_enabled(lv_obj_has_state(album_view_switch, LV_STATE_CHECKED));
	actions();
}

static void auto_connect_cb(lv_event_t *event) {
	(void)event;
	if (!gesture()) {
		config_set_bool("navidrome", "auto_connect", lv_obj_has_state(auto_connect_switch, LV_STATE_CHECKED));
		config_save();
	}
	actions();
}

static void downloads_cb(lv_event_t *e) { (void)e; if (!gesture()) navidromedownloadpage_open(); }

void navidromesettings_init(gui_config_t *cfg) {
	config = cfg;
	navidromedownloadpage_init(cfg);
	form_init(&settings_form, "nd_settings");
	form_init(&main_form, "nd_server");
	settingsrow_add(settings_form.body, "nd_server", NULL, server_cb, NULL);
	settingsrow_add(settings_form.body, "nd_dl_downloads", NULL, downloads_cb, NULL);
	settingsrow_toggle(settings_form.body, "nd_auto_connect", &auto_connect_switch, auto_connect_cb);
	lv_obj_t *auto_note = lv_label_create(settings_form.body);
	lv_obj_set_width(auto_note, lv_pct(100));
	lv_label_set_long_mode(auto_note, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(auto_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(auto_note, &font_ui_18, 0);
	lv_label_set_text(auto_note, tr("nd_auto_connect_note"));
	settingsrow_toggle(settings_form.body, "musicsettings_album_head", &album_view_switch, album_view_cb);
	lv_obj_t *album_note = lv_label_create(settings_form.body);
	lv_obj_set_width(album_note, lv_pct(100));
	lv_label_set_long_mode(album_note, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(album_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(album_note, &font_ui_18, 0);
	lv_label_set_text(album_note, tr("musicsettings_album_head_note"));
	encrypt_row = settingsrow_toggle(settings_form.body, "nd_encrypt_credentials", &encrypt_switch, encrypt_cb);
	protection_note = lv_label_create(settings_form.body);
	lv_obj_set_width(protection_note, lv_pct(100));
	lv_obj_add_style(protection_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(protection_note, &font_ui_18, 0);
	make_field(&main_form, 0, "nd_host", false, ND_HOST_MAX - 1);
	lv_textarea_set_placeholder_text(main_form.fields[0], "http://music.local:4533");
	make_field(&main_form, 1, "nd_username", false, ND_USER_MAX - 1);
	make_field(&main_form, 2, "password", true, ND_SECRET_MAX - 1);
	save_button = action(&main_form, "nd_save_connect", save_cb);
	update_button = action(&main_form, "nd_update_password", save_cb);
	reconnect_button = action(&settings_form, "nd_reconnect", reconnect_cb);
	unlock_button = action(&settings_form, "nd_unlock", unlock_cb);
	lock_button = action(&settings_form, "nd_lock", lock_cb);
	change_button = action(&settings_form, "nd_change_passphrase", change_cb);
	forget_button = action(&main_form, "nd_forget", forget_cb);
	library_status = lv_label_create(settings_form.body);
	sync_status = lv_label_create(settings_form.body);
	lv_obj_t *labels[] = {library_status, sync_status};
	for (int i = 0; i < 2; i++) {
		lv_obj_set_width(labels[i], lv_pct(100));
		lv_label_set_long_mode(labels[i], LV_LABEL_LONG_WRAP);
		lv_obj_add_style(labels[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(labels[i], &font_ui_18, 0);
	}
	sync_button = action(&settings_form, "nd_sync_library", sync_cb);
	sync_cancel_button = action(&settings_form, "nd_sync_cancel", sync_cancel_cb);
	cache_card_build();
	settingsrow_toggle(settings_form.body, "nd_debug_mode", &debug_switch, debug_cb);
	debug_note = lv_label_create(settings_form.body);
	lv_obj_set_width(debug_note, lv_pct(100));
	lv_label_set_long_mode(debug_note, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(debug_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(debug_note, &font_ui_18, 0);
	nd_debug_enable(config_get_bool("navidrome", "debug", false));
	main_form.keyboard = keyboard_create(main_form.screen, cfg->screen_width, ui_px(316), main_form.fields[0], NULL, "ok", save_cb, NULL);
	keyboard_visible(&main_form, false);
	lv_obj_add_event_cb(main_form.screen, main_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(main_form.screen, main_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(settings_form.screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	form_init(&pass_form, "nd_vault");
	make_field(&pass_form, 0, "nd_current_passphrase", true, ND_SECRET_MAX - 1);
	make_field(&pass_form, 1, "nd_new_passphrase", true, ND_SECRET_MAX - 1);
	make_field(&pass_form, 2, "nd_confirm_passphrase", true, ND_SECRET_MAX - 1);
	pass_button = action(&pass_form, "ok", pass_accept_cb);
	pass_form.keyboard = keyboard_create(pass_form.screen, cfg->screen_width, ui_px(316), pass_form.fields[0], NULL, "ok", pass_accept_cb, NULL);
	keyboard_visible(&pass_form, false);
	lv_obj_add_event_cb(pass_form.screen, pass_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	refresh_timer = lv_timer_create(refresh_tick, 250, NULL);
	power_slow_in_standby(refresh_timer, 1000);
	lv_timer_pause(refresh_timer);
	nd_cache_set_listener(navidromesettings_refresh);
}
