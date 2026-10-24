#ifndef NAVIDROMEVAULT_H
#define NAVIDROMEVAULT_H

#include "navidromecrypto.h"

#define ND_HOST_MAX 512
#define ND_USER_MAX 128
#define ND_SECRET_MAX 512
#define ND_UNLOCK_PASSWORD_MIN_CHARS 6
#define ND_KDF_ITERATIONS 600000u
#define ND_KDF_MAX_ITERATIONS 2400000u
#define ND_VAULT_FILE "navidrome_credentials.json"

typedef struct {
	char host[ND_HOST_MAX];
	char user[ND_USER_MAX];
	bool authenticated;
	bool plain;
	unsigned version;
	unsigned iterations;
	// cipher holds ciphertext in encrypted mode, an owned NUL-terminated
	// password in plain mode. Clear the whole record after worker use.
	uint8_t salt[16], nonce[12], tag[16], cipher[ND_SECRET_MAX];
	size_t size;
} nd_record_t;

// All disk operations are blocking; the service serializes them. Errors are
// localization keys only, never contents of a password or request URL.
bool nd_vault_path(const char *config, char *out, size_t size);
int nd_vault_load(const char *path, nd_record_t *record); // 0 missing, 1 valid, -1 invalid/unreadable
bool nd_passphrase_valid(const char *text);
bool nd_vault_prepare(nd_record_t *record, const char *passphrase, uint8_t key[32]);
bool nd_vault_plain(nd_record_t *record, const char *password);
bool nd_vault_seal(nd_record_t *record, const uint8_t key[32], const char *password);
bool nd_vault_open(const nd_record_t *record, const uint8_t key[32], char password[ND_SECRET_MAX]);
int nd_vault_save(const char *path, const nd_record_t *record); // 0 failed, 1 committed, 2 committed but directory sync failed
// Service splits slow staging/sync from the short generation-guarded rename.
bool nd_vault_stage(const char *path, const nd_record_t *record);
bool nd_vault_commit(const char *path);
bool nd_vault_sync(const char *path);
void nd_vault_discard(const char *path);
bool nd_vault_remove(const char *path);

#endif
