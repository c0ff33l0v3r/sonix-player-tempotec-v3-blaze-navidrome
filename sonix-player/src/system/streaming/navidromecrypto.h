#ifndef NAVIDROMECRYPTO_H
#define NAVIDROMECRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Small libcrypto boundary, independent of the TLS client and its CA bundle.
void nd_wipe(void *data, size_t size);
bool nd_random(void *data, size_t size);
bool nd_derive(const char *passphrase, const uint8_t salt[16], unsigned iterations, uint8_t key[32]);
bool nd_encrypt(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len, const void *plain, size_t size, uint8_t *cipher, uint8_t tag[16]);
bool nd_decrypt(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len, const uint8_t *cipher, size_t size, const uint8_t tag[16], void *plain);

#endif
