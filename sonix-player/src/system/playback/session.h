#ifndef PLAYBACK_SESSION_H
#define PLAYBACK_SESSION_H
#include <stdbool.h>
// GUI-owned admission; worker-owned persistence. Init after card/GUI setup.
void playback_session_init(bool (*legacy_restore)(void));
void playback_session_note(void);
void playback_session_settings(void);
void playback_session_service_changed(void) __attribute__((weak)); // GUI event, no periodic readiness poll
void playback_session_reset(void); // stop/cancel before Factory Reset deletes files
void playback_session_flush(void);
void playback_session_forget(void);
void playback_session_suspend(void); // GUI after live provider revocation
bool playback_session_blocked(void);
// Non-restorable sources fence startup restore immediately; GUI delivery is deferred.
void playback_session_other_started(void);
#endif
