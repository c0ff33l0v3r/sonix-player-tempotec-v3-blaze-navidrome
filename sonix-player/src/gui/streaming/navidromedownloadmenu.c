#include "navidromedownloadmenu.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/navidromedownloads.h"
#include "src/system/streaming/navidromeplay.h"
#include <stdio.h>
#include <string.h>

static nd_dl_request_t target;
static nd_context_t identity;
static unsigned menu_serial;
static bool target_current(void) {
	device_state_t state;
	nd_playrow_t row;
	nd_library_t lib;
	device_state_get(&state);
	return nd_audio_source(state.current_file) && navidrome_playback_current(&identity, false) && navidromeplay_track(state.current_file, &row, &lib) && !strcmp(row.row.id, target.id) && lib.card_epoch == target.library.card_epoch && !strcmp(lib.scope, target.library.scope);
}
static void captured(bool ok, const char *error, int added, int skipped, void *user) {
	if ((unsigned)(uintptr_t)user != menu_serial || !target_current())
		return;
	if (!ok) {
		gui_notify_popup(error ? error : "nd_dl_capture_error");
		return;
	}
	char text[160];
	lv_snprintf(text, sizeof(text), tr("nd_dl_admitted"), added, skipped);
	toast_success(text);
}
void navidromedownloadmenu_activate(void *unused) {
	(void)unused;
	if (!target_current()) {
		gui_notify_popup("nd_dl_unavailable");
		return;
	}
	const char *folder = config_get("navidrome", "download_folder", "Music/Navidrome");
	if (!nd_dl_destination(folder)) {
		gui_notify_popup("nd_dl_path_error");
		return;
	}
	snprintf(target.destination, sizeof(target.destination), "%s", folder);
	if (!navidromedownloads_request(&target, captured, (void *)(uintptr_t)menu_serial))
		gui_notify_popup("nd_working");
}
bool navidromedownloadmenu_bind(const char *path) {
	menu_serial++;
	nd_wipe(&identity, sizeof(identity));
	memset(&target, 0, sizeof(target));
	bool ok = nd_audio_source(path) && navidromeplay_track(path, &target.track, &target.library) && navidrome_context(&identity, false);
	// The controller needs lifecycle stamps, never credential copies.
	nd_wipe(identity.host, sizeof(identity.host));
	nd_wipe(identity.user, sizeof(identity.user));
	nd_wipe(identity.password, sizeof(identity.password));
	if (ok) {
		target.action = NDDL_CAPTURE;
		target.kind = ND_TRACKS;
		target.direct = true;
		target.profile_epoch = identity.profile_epoch;
		snprintf(target.id, sizeof(target.id), "%s", target.track.row.id);
	}
	return ok;
}
