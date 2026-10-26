#include "navidromestar.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/navidromeplay.h"
#include "src/system/streaming/navidromestars.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	lv_obj_t *origin;
	void (*refresh)(void);
} player_action_t;

bool navidromestar_player_update(lv_obj_t *button, lv_obj_t *icon, const char *reference, lv_color_t ink) {
	if (!nd_audio_source(reference))
		return false;
	nd_playrow_t track;
	bool valid = navidromeplay_track(reference, &track, NULL);
	nd_star_state_t star = valid ? navidrome_star_state(ND_TRACKS, track.row.id) : ND_STAR_UNKNOWN;
	if (star == ND_STAR_UNKNOWN && valid && (track.row.favorite_known || track.row.favorite))
		star = track.row.favorite ? ND_STAR_SET : ND_STAR_CLEAR;
	if (button)
		lv_obj_set_state(button, LV_STATE_DISABLED, !valid || navidrome_star_busy());
	if (icon) {
		lv_image_set_src(icon, star == ND_STAR_SET ? &icon_star_filled : &icon_star);
		lv_obj_set_style_image_recolor(icon, star == ND_STAR_SET ? lv_color_make(246, 211, 45) : ink, 0);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
		lv_obj_set_style_opa(icon, star == ND_STAR_SET || star == ND_STAR_CLEAR ? LV_OPA_COVER : LV_OPA_40, 0);
	}
	return true;
}
static void player_done(nd_star_result_t *r, void *user) {
	player_action_t *action = user;
	device_state_t current;
	device_state_get(&current);
	nd_playrow_t track;
	if (navidrome_star_current(&r->context) && navidromeplay_track(current.current_file, &track, NULL) && !strcmp(track.row.id, r->request.id)) {
		// Match Music: the star is the success feedback; report errors only.
		if (r->error && lv_screen_active() == action->origin)
			gui_notify_popup(r->error);
		if (action->refresh)
			action->refresh();
	}
	free(action);
	free(r);
}
bool navidromestar_player_toggle(const char *reference, void (*refresh)(void)) {
	if (!nd_audio_source(reference))
		return false;
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active())
		return true;
	nd_context_t captured;
	if (!navidrome_context(&captured, false)) {
		navidrome_snapshot_t snapshot;
		navidrome_snapshot(&snapshot);
		gui_notify_popup(navidrome_feedback(&snapshot));
		return true;
	}
	unsigned profile = captured.profile_epoch;
	nd_wipe(&captured, sizeof(captured));
	nd_playrow_t track;
	if (!navidromeplay_track(reference, &track, NULL)) {
		gui_notify_popup("nd_audio_unavailable");
		return true;
	}
	player_action_t *action = malloc(sizeof(*action));
	if (!action) {
		gui_notify_popup("nd_worker_error");
		return true;
	}
	*action = (player_action_t){.origin = lv_screen_active(), .refresh = refresh};
	nd_star_request_t q = {.action = navidrome_star_retry(ND_TRACKS, track.row.id) ? ND_STAR_RETRY : ND_STAR_TOGGLE, .metadata = track, .kind = ND_TRACKS, .bound_profile = true, .profile_epoch = profile};
	snprintf(q.id, sizeof(q.id), "%s", track.row.id);
	if (!navidrome_star_submit(&q, player_done, action)) {
		free(action);
		navidrome_snapshot_t snapshot;
		navidrome_snapshot(&snapshot);
		gui_notify_popup(snapshot.state == ND_CONNECTED ? "nd_working" : navidrome_feedback(&snapshot));
	}
	if (refresh)
		refresh();
	return true;
}
