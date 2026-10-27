#ifndef CONFIRM_H
#define CONFIRM_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// A modal yes/no card over a dimmed page: the shape every "are you sure?"
// in the interface uses. One exists at a time; asking again replaces it.
//
// The action runs after the card has closed, so it can switch screens or
// start a long job without pulling the dialog out from under itself.

void confirm_init(gui_config_t *cfg);

// `message` may be NULL for a title-only question. `ok_label` is the accent
// button's text ("Scan", "Delete", ...); NULL uses "OK".
void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user),
				  void *user);

// The same card with one button: something the user has to be told, not asked.
// Dismissing it does nothing beyond closing it.
void confirm_notice(const char *title, const char *message);

// Optional labels/eligibility for a two-choice prompt; existing callers keep defaults.
void confirm_show_choice(const char *title, const char *message, const char *yes, const char *no, bool enabled, void (*on_yes)(void *), void (*on_no)(void *), void *user);
bool confirm_visible(void);
void confirm_close(void);

// A handle identifies this displayed prompt, without changing legacy callers.
unsigned confirm_show_owned(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *), void (*on_cancel)(void *), void *user);
void confirm_close_owned(unsigned owner);
bool confirm_is_current(unsigned owner);

#endif /* CONFIRM_H */
