static void search_actions_poll(lv_timer_t *timer);
static void search_actions_notice(void *unused) { (void)unused; search_actions_poll(NULL); }
// Private Search controller. Service requests own all data before row recycling.
typedef enum { SEARCH_MARKS, SEARCH_TOGGLE, SEARCH_PLAYLIST, SEARCH_QUEUE, SEARCH_DOWNLOAD, SEARCH_FAVORITE } search_action_t;
typedef struct {
	search_page_t *page;
	unsigned visit, serial, queue;
	search_action_t action;
	nd_kind_t kind;
	nd_star_action_t star;
	nd_playrow_t row;
} search_action_context_t;
static search_page_t *search_menu_page;
static unsigned search_menu_visit, search_menu_serial, search_menu_token;
static int search_menu_index;
static nd_star_action_t search_menu_star;
static void search_selection_ui(search_page_t *p) {
	if (p->marks_dirty) gui_post_signal(search_actions_notice, NULL);
	bool selecting = p->selected > 0;
	for (int slot = 0; slot < 5; slot++) {
		lv_obj_set_hidden(p->select_buttons[slot], !selecting);
		lv_obj_set_state(p->select_buttons[slot], LV_STATE_DISABLED, slot && p->action_busy);
		if (selecting)
			lv_obj_move_foreground(p->select_buttons[slot]);
		navidromealbum_icon(&p->album_view, lv_obj_get_child(p->select_buttons[slot], 0));
	}
	lv_obj_set_hidden(p->selection_status, !selecting);
	lv_obj_set_hidden(p->title, selecting && !p->album_view.active);
	if (selecting) {
		char text[80];
		lv_snprintf(text, sizeof(text), tr("nd_pl_selected"), p->selected);
		lv_label_set_text(p->selection_status, text);
		lv_obj_set_style_bg_color(p->selection_status, p->album_view.tinted ? p->album_view.bg : theme()->screen_bg, 0);
		lv_obj_set_style_text_color(p->selection_status, p->album_view.tinted ? p->album_view.ink : theme()->text_primary, 0);
		lv_obj_move_foreground(p->selection_status);
	}
	for (int i = 0; i < ND_WINDOW; i++) {
		bool heading = p->album_view.active && p->data.album.heading[i];
		lv_obj_set_hidden(p->menus[i], selecting || heading || !p->data.rows[i].id[0]);
		lv_obj_set_state(p->menus[i], LV_STATE_DISABLED, p->action_busy);
		lv_obj_set_hidden(p->checks[i], !selecting || !p->chosen[i] || heading);
		if (selecting)
			lv_obj_set_style_border_color(p->rows[i], theme()->accent, 0);
		lv_obj_set_style_border_width(p->rows[i], selecting && p->chosen[i] ? ui_px(2) : 0, 0);
		navidromealbum_icon(&p->album_view, lv_obj_get_child(p->menus[i], 0));
		navidromealbum_icon(&p->album_view, p->checks[i]);
	}
}
static void search_selection_clear(search_page_t *p) {
	p->visit++;
	p->selected = 0;
	p->action_busy = false;
	p->marks_dirty = false;
	memset(p->chosen, 0, sizeof(p->chosen));
	nd_owned_release(p->selection);
	p->selection = NULL;
	search_menu_token++;
	popover_close();
	if (p->selection_status)
		search_selection_ui(p);
}
static bool search_selection_peek(void) {
	int level = search_active_page();
	return level >= 0 && search_pages[level].selected > 0;
}
static bool search_selection_back(void) {
	int level = search_active_page();
	if (level < 0 || !search_pages[level].selected)
		return false;
	search_page_t *p = &search_pages[level];
	search_selection_clear(p);
	search_layout(p);
	return true;
}
static bool search_action_visible(const search_action_context_t *ctx) { return ctx->visit == ctx->page->visit && lv_screen_active() == ctx->page->screen && navidrome_search_current(&search_identity, false); }
static void search_download_done(bool ok, const char *error, int added, int skipped, void *value) {
	search_action_context_t *ctx = value;
	if (search_action_visible(ctx)) {
		ctx->page->action_busy = false;
		if (ok) {
			char text[160];
			lv_snprintf(text, sizeof(text), tr("nd_dl_admitted"), added, skipped);
			toast_success(text);
		} else
			gui_notify_popup(error ? error : "nd_dl_capture_error");
		search_selection_ui(ctx->page);
	}
	free(ctx);
}
static void search_star_done(nd_star_result_t *result, void *value) {
	search_action_context_t *ctx = value;
	if (search_action_visible(ctx)) {
		ctx->page->action_busy = false;
		if (result->error) {
			if (result->request.action == ND_STAR_SELECTED) {
				char message[384], summary[160];
				lv_snprintf(summary, sizeof(summary), tr("nd_favorites_batch"), result->completed, result->total);
				lv_snprintf(message, sizeof(message), "%s\n%s", summary, tr(result->error));
				gui_notify_popup(message);
			} else
				gui_notify_popup(result->error);
		} else
			toast_success("nd_favorites_saved");
		search_selection_ui(ctx->page);
	}
	free(result);
	free(ctx);
}
static void search_action_done(nd_action_result_t *result, void *value) {
	gui_post_signal(search_actions_notice, NULL);
	search_action_context_t *ctx = value;
	search_page_t *p = ctx->page;
	bool visible = search_action_visible(ctx);
	if (visible)
		p->action_busy = false;
	if (ctx->action == SEARCH_MARKS || ctx->action == SEARCH_TOGGLE) {
		if (visible) {
			nd_owned_release(p->selection);
			p->selection = result->selection;
			result->selection = NULL;
			p->selected = result->selected;
			if (p->serial == ctx->serial) {
				memcpy(p->chosen, result->marks, sizeof(p->chosen));
				p->marks_dirty = false;
			} else
				p->marks_dirty = p->selected > 0;
			if (result->error)
				gui_notify_popup(result->error);
			search_selection_ui(p);
			search_layout(p);
		}
		nd_action_result_free(result);
		free(ctx);
		return;
	}
	bool ok = !result->error && nd_owned_current(result->capture);
	if (ok && (ctx->action == SEARCH_PLAYLIST || ctx->action == SEARCH_DOWNLOAD) && !result->capture->library.writable) {
		ok = false;
		result->error = result->capture->library.path[0] ? "nd_cache_readonly" : "nd_cache_storage";
	}
	if (ok && ctx->action == SEARCH_PLAYLIST) {
		if (visible) {
			ok = navidromeplaylists_pick_captured(result->capture, p->screen);
		}
	} else if (ok && ctx->action == SEARCH_QUEUE) {
		if (!result->capture->library.path[0]) {
			ok = false;
			result->error = "nd_cache_storage";
		} else
			ok = navidromeplay_enqueue_captured(result->capture, ctx->queue, p->screen);
		if (!visible)
			navidromeplay_playlist_leave(p->screen);
	} else if (ok && ctx->action == SEARCH_DOWNLOAD) {
		nd_dl_request_t request = {.action = NDDL_CAPTURE, .library = result->capture->library, .captured = result->capture, .profile_epoch = result->capture->identity.profile_epoch};
		const char *folder = config_get("navidrome", "download_folder", "Music/Navidrome");
		ok = strlen(folder) < sizeof(request.destination);
		if (ok) {
			snprintf(request.destination, sizeof(request.destination), "%s", folder);
			ok = navidromedownloads_request(&request, search_download_done, ctx);
		}
		if (ok) {
			if (visible)
				p->action_busy = true;
			ctx = NULL;
		}
	} else if (ok && ctx->action == SEARCH_FAVORITE) {
		navidrome_snapshot_t state;
		navidrome_snapshot(&state);
		nd_star_request_t request = {.kind = ctx->kind, .action = ctx->star, .captured = result->capture, .bound_profile = true, .profile_epoch = result->capture->identity.profile_epoch, .metadata = ctx->row};
		if (ctx->star != ND_STAR_SELECTED)
			snprintf(request.id, sizeof(request.id), "%s", ctx->row.row.id);
		ok = navidrome_star_submit(&request, search_star_done, ctx);
		if (!ok && state.state != ND_CONNECTED)
			result->error = "nd_reconnect_required";
		if (ok) {
			if (visible)
				p->action_busy = true;
			ctx = NULL;
		}
	}
	if (visible && !ok)
		gui_notify_popup(result->error ? result->error : "nd_working");
	if (visible)
		search_selection_ui(p);
	nd_action_result_free(result);
	free(ctx);
}
static bool search_action_start(search_page_t *p, search_action_t action, int index, nd_star_action_t star) {
	if (search_gesture() || p->loading || p->action_busy || navidrome_actions_busy() || p->data.error || !navidrome_search_current(&search_identity, false))
		return false;
	if ((index < 0 && !p->selection) || index >= p->data.count)
		return false;
	search_action_context_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		gui_notify_popup("nd_json_memory");
		return false;
	}
	*ctx = (search_action_context_t){.page = p, .visit = p->visit, .serial = p->serial, .queue = playlist_identity(), .action = action, .kind = p->kind, .star = star};
	nd_action_request_t request = {.op = action == SEARCH_TOGGLE ? ND_ACTION_TOGGLE : action == SEARCH_MARKS ? ND_ACTION_MARKS : ND_ACTION_CAPTURE, .library = p->data.library, .profile_epoch = search_identity.profile_epoch, .kind = p->kind, .remote = p->data.source == ND_SEARCH_SERVER, .expand = action != SEARCH_FAVORITE, .count = p->data.count};
	if (action == SEARCH_TOGGLE || action == SEARCH_MARKS || index < 0)
		request.selection = p->selection;
	if (index >= 0) {
		request.row.row = p->data.rows[index];
		if (!request.row.row.id[0]) {
			free(ctx);
			return false;
		}
		if (p->kind == ND_TRACKS && p->album[0]) {
			snprintf(request.row.album_id, sizeof(request.row.album_id), "%s", p->album);
			snprintf(request.row.album, sizeof(request.row.album), "%s", p->data.album.info.row.title);
			snprintf(request.row.album_artist, sizeof(request.row.album_artist), "%s", p->data.album.info.album_artist);
		}
		ctx->row = request.row;
		request.ordinal = p->album_view.active ? p->data.album.ordinal[index] : p->data.first + index;
	}
	for (int i = 0; i < request.count; i++)
		snprintf(request.ids[i], sizeof(request.ids[i]), "%s", p->data.rows[i].id);
	for (int level = 1; level < 3; level++) {
		request.albums[level - 1] = search_pages[level].child_rows;
		memcpy(request.album_ids[level - 1], search_pages[level].retained_album, ND_ID_MAX);
	}
	if (!navidrome_actions_submit(&request, search_action_done, ctx)) {
		free(ctx);
		return false;
	}
	p->action_busy = true;
	search_selection_ui(p);
	return true;
}
static void search_action_button(lv_event_t *event) {
	search_page_t *p = lv_event_get_user_data(event);
	if (search_gesture())
		return;
	lv_obj_t *button = lv_event_get_current_target(event);
	if (button == p->select_buttons[0]) {
		search_selection_back();
		return;
	}
	search_action_t action = button == p->select_buttons[1] ? SEARCH_PLAYLIST : button == p->select_buttons[2] ? SEARCH_FAVORITE : button == p->select_buttons[3] ? SEARCH_QUEUE : SEARCH_DOWNLOAD;
	if (!search_action_start(p, action, -1, ND_STAR_SELECTED))
		gui_notify_popup("nd_working");
}
static void search_long_pressed(lv_event_t *event) {
	search_page_t *p = lv_event_get_user_data(event);
	if (search_gesture() || p->kind == ND_ARTISTS)
		return;
	for (int i = 0; i < p->data.count; i++)
		if (p->rows[i] == lv_event_get_current_target(event)) {
			lv_indev_t *indev = lv_indev_active();
			if (indev)
				lv_indev_wait_release(indev);
			if (!p->chosen[i])
				search_action_start(p, SEARCH_TOGGLE, i, ND_STAR_ADD);
			return;
		}
}
static void search_menu_action(void *value) {
	uintptr_t token = (uintptr_t)value;
	search_page_t *p = search_menu_page;
	if (!p || token >> 3 != search_menu_token || p->visit != search_menu_visit || p->serial != search_menu_serial || lv_screen_active() != p->screen)
		return;
	if (!search_action_start(p, (search_action_t)(token & 7), search_menu_index, search_menu_star))
		gui_notify_popup("nd_working");
}
static void search_menu_clicked(lv_event_t *event) {
	lv_event_stop_bubbling(event);
	search_page_t *p = lv_event_get_user_data(event);
	if (search_gesture() || p->loading || p->action_busy || p->selected)
		return;
	for (int i = 0; i < p->data.count; i++)
		if (p->menus[i] == lv_event_get_current_target(event) && p->data.rows[i].id[0]) {
			search_menu_page = p;
			search_menu_index = i;
			search_menu_visit = p->visit;
			search_menu_serial = p->serial;
			search_menu_token++;
			nd_star_state_t state = navidrome_star_state(p->kind, p->data.rows[i].id);
			bool favorite = state == ND_STAR_SET || ((state == ND_STAR_UNKNOWN || state == ND_STAR_UNCERTAIN) && p->data.rows[i].favorite);
			search_menu_star = state == ND_STAR_UNCERTAIN ? ND_STAR_TOGGLE : favorite ? ND_STAR_REMOVE : ND_STAR_ADD;
			popover_item_t items[4];
			int n = 0;
			uintptr_t base = (uintptr_t)search_menu_token << 3;
			items[n++] = (popover_item_t){favorite ? "remove_from_favourites" : "medialist_add_to_favourites", search_menu_action, (void *)(base | SEARCH_FAVORITE), false};
			if (p->kind != ND_ARTISTS) {
				items[n++] = (popover_item_t){"add_to_queue", search_menu_action, (void *)(base | SEARCH_QUEUE), false};
				items[n++] = (popover_item_t){"add_to_playlist", search_menu_action, (void *)(base | SEARCH_PLAYLIST), false};
				items[n++] = (popover_item_t){"nd_dl_download", search_menu_action, (void *)(base | SEARCH_DOWNLOAD), false};
			}
			popover_show(p->menus[i], items, n);
			return;
		}
}
static void search_actions_poll(lv_timer_t *timer) {
	(void)timer;
	navidrome_actions_poll();
	navidrome_playlists_poll();
	navidrome_star_poll();
	navidromedownloads_poll();
	int level = search_active_page();
	if (level >= 0) {
		search_page_t *p = &search_pages[level];
		if (p->marks_dirty && p->selection && !p->loading && !p->action_busy && !navidrome_actions_busy())
			search_action_start(p, SEARCH_MARKS, -1, ND_STAR_ADD);
	}
}
static void search_actions_init(search_page_t *p, gui_config_t *cfg) {
	for (int slot = 0; slot < 5; slot++) {
		lv_obj_t *button = p->select_buttons[slot] = lv_btn_create(p->screen);
		lv_obj_set_size(button, ui_px(48), ui_px(48));
		lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * ui_px(50), cfg->padding + cfg->top_bar_height);
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_set_style_shadow_width(button, 0, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_t *glyph = lv_image_create(button);
		lv_image_set_src(glyph, slot == 4 ? &icon_download : slot == 3 ? &icon_list_queue : slot == 2 ? &icon_star_plus : slot == 1 ? &icon_list_plus : &icon_close);
		lv_obj_add_style(glyph, &theme_style_icon, 0);
		lv_obj_center(glyph);
		lv_obj_add_event_cb(button, search_action_button, LV_EVENT_CLICKED, p);
		switcher_attach_back_gesture(button);
		player_sheet_attach_drag(button, true);
		lv_obj_set_hidden(button, true);
	}
	p->selection_status = lv_label_create(p->screen);
	lv_obj_set_pos(p->selection_status, cfg->padding, p->status_top);
	lv_obj_set_width(p->selection_status, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(p->selection_status, ui_px(28));
	lv_obj_set_style_text_font(p->selection_status, &font_ui_18, 0);
	lv_obj_set_style_bg_opa(p->selection_status, LV_OPA_COVER, 0);
	lv_obj_set_hidden(p->selection_status, true);
	for (int i = 0; i < ND_WINDOW; i++) {
		lv_obj_t *button = p->menus[i] = nd_row_menu_create(p->rows[i]);
		lv_obj_set_style_pad_all(button, 0, 0);
		p->checks[i] = lv_image_create(p->rows[i]);
		lv_image_set_src(p->checks[i], &icon_check);
		lv_obj_add_style(p->checks[i], &theme_style_icon, 0);
		lv_obj_align(p->checks[i], LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_set_hidden(p->checks[i], true);
		lv_obj_add_event_cb(button, search_menu_clicked, LV_EVENT_CLICKED, p);
		lv_obj_add_event_cb(p->rows[i], search_long_pressed, LV_EVENT_LONG_PRESSED, p);
	}
	switcher_set_back_guard(p->screen, search_selection_back);
	switcher_set_back_guard_peek(p->screen, search_selection_peek);
}
