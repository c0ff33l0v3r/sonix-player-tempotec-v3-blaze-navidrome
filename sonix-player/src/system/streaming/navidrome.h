#ifndef NAVIDROME_H
#define NAVIDROME_H

#include "navidromestore.h"
#include "navidromevault.h"

typedef enum { ND_UNCONFIGURED, ND_LOCKED, ND_DISCONNECTED, ND_CONNECTING, ND_CONNECTED } navidrome_state_t;
typedef struct {
	char host[ND_HOST_MAX], user[ND_USER_MAX];
	bool configured, unlocked, authenticated, invalid, busy;
	navidrome_state_t state;
	unsigned revision;
	const char *error; // localization key; never a credential-bearing server response
} navidrome_snapshot_t;

typedef struct {
	char host[ND_HOST_MAX], user[ND_USER_MAX], password[ND_SECRET_MAX];
	unsigned epoch, card_epoch, network_epoch;
} nd_context_t;

void navidrome_init(void);
void navidrome_shutdown(void);
void navidrome_set_listener(void (*changed)(void)); // listener runs on GUI thread
void navidrome_snapshot(navidrome_snapshot_t *out);
bool navidrome_password(char out[ND_SECRET_MAX]);
const char *navidrome_validate(const char *host, const char *user, const char *password, char normalized[ND_HOST_MAX]);
bool navidrome_save(const char *host, const char *user, const char *password, const char *new_passphrase);
bool navidrome_update_password(const char *host, const char *user, const char *password);
bool navidrome_unlock(const char *passphrase);
bool navidrome_change_passphrase(const char *current, const char *replacement);
bool navidrome_reconnect(void);
void navidrome_lock(void);
bool navidrome_forget(void);
void navidrome_poll_network(void); // local snapshot only; no request, no host wlan0 requirement
void navidrome_reset(void);		   // explicit factory reset, serializes removal against commits
const char *navidrome_feedback(const navidrome_snapshot_t *snapshot);

void navidrome_set_root(const char *root);
void navidrome_library_snapshot(nd_library_t *out);
bool navidrome_sync(void);
void navidrome_sync_cancel(void);
void navidrome_sync_offer_consume(unsigned offer);
bool navidrome_context(nd_context_t *out, bool network);
// Short card-I/O leases let removal drain open files without waiting for HTTP.
bool navidrome_cache_begin(unsigned epoch, unsigned card_epoch);
void navidrome_cache_end(void);
bool navidrome_context_publish(const nd_context_t *context, const char *temp, const char *path);
bool navidrome_context_current(const nd_context_t *context, bool network);

// Blocking protocol boundary, also used by the focused fixture checks.
const char *navidrome_ping(const char *host, const char *user, const char *password);
#ifdef NAVIDROME_TEST
void navidrome_test_online(int online); // -1 production behavior, 0/1 injected device snapshot
#endif
#endif
