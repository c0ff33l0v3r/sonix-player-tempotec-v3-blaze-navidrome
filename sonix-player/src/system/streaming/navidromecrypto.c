#include "navidromecrypto.h"

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>

// Opaque, stable OpenSSL 1.1/3 ABI, as in net/tls.c. The MIPS toolchain
// lacks OpenSSL headers; no home-grown cryptographic primitive lives here.
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct evp_cipher_st EVP_CIPHER;
typedef struct evp_md_st EVP_MD;
static struct {
	EVP_CIPHER_CTX *(*EVP_CIPHER_CTX_new)(void);
	void (*EVP_CIPHER_CTX_free)(EVP_CIPHER_CTX *);
	const EVP_CIPHER *(*EVP_aes_256_gcm)(void);
	const EVP_MD *(*EVP_sha256)(void);
	int (*EVP_CipherInit_ex)(EVP_CIPHER_CTX *, const EVP_CIPHER *, void *, const unsigned char *, const unsigned char *, int);
	int (*EVP_CipherUpdate)(EVP_CIPHER_CTX *, unsigned char *, int *, const unsigned char *, int);
	int (*EVP_CipherFinal_ex)(EVP_CIPHER_CTX *, unsigned char *, int *);
	int (*EVP_CIPHER_CTX_ctrl)(EVP_CIPHER_CTX *, int, int, void *);
	int (*PKCS5_PBKDF2_HMAC)(const char *, int, const unsigned char *, int, int, const EVP_MD *, int, unsigned char *);
	int (*RAND_bytes)(unsigned char *, int);
} api;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static bool available;

static void load_crypto(void) {
	const char *names[] = {"libcrypto.so.1.1", "libcrypto.so.3", "libcrypto.so", NULL};
	void *lib = NULL;
	for (int i = 0; names[i] && !lib; i++)
		lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
	if (!lib)
		return;
#define BIND(name)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     \
	do {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               \
		void *p = dlsym(lib, #name);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   \
		if (!p) {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      \
			dlclose(lib);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
			return;                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    \
		}                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
		memcpy(&api.name, &p, sizeof(p));                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
	} while (0)
	BIND(EVP_CIPHER_CTX_new);
	BIND(EVP_CIPHER_CTX_free);
	BIND(EVP_aes_256_gcm);
	BIND(EVP_sha256);
	BIND(EVP_CipherInit_ex);
	BIND(EVP_CipherUpdate);
	BIND(EVP_CipherFinal_ex);
	BIND(EVP_CIPHER_CTX_ctrl);
	BIND(PKCS5_PBKDF2_HMAC);
	BIND(RAND_bytes);
#undef BIND
	available = true; // Keep the library loaded while any worker may use it.
}

void nd_wipe(void *data, size_t size) {
	volatile unsigned char *p = data;
	while (size--)
		*p++ = 0;
}

bool nd_random(void *data, size_t size) {
	pthread_once(&once, load_crypto);
	return available && size <= INT_MAX && api.RAND_bytes(data, (int)size) == 1;
}

bool nd_derive(const char *passphrase, const uint8_t salt[16], unsigned iterations, uint8_t key[32]) {
	pthread_once(&once, load_crypto);
	if (!available || !passphrase || strlen(passphrase) > INT_MAX || !iterations || iterations > INT_MAX)
		return false;
	bool ok = api.PKCS5_PBKDF2_HMAC(passphrase, (int)strlen(passphrase), salt, 16, (int)iterations, api.EVP_sha256(), 32, key) == 1;
	if (!ok)
		nd_wipe(key, 32);
	return ok;
}

static bool crypt(bool encrypt, const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len, const void *in, size_t size, void *out, uint8_t tag[16]) {
	pthread_once(&once, load_crypto);
	if (!available || size > INT_MAX || aad_len > INT_MAX)
		return false;
	EVP_CIPHER_CTX *ctx = api.EVP_CIPHER_CTX_new();
	if (!ctx)
		return false;
	int n = 0, tail = 0;
	bool ok = api.EVP_CipherInit_ex(ctx, api.EVP_aes_256_gcm(), NULL, NULL, NULL, encrypt) == 1 && api.EVP_CIPHER_CTX_ctrl(ctx, 0x9 /* SET_IVLEN */, 12, NULL) == 1 && api.EVP_CipherInit_ex(ctx, NULL, NULL, key, nonce, encrypt) == 1 && api.EVP_CipherUpdate(ctx, NULL, &n, aad, (int)aad_len) == 1 && api.EVP_CipherUpdate(ctx, out, &n, in, (int)size) == 1;
	if (ok && !encrypt)
		ok = api.EVP_CIPHER_CTX_ctrl(ctx, 0x11 /* SET_TAG */, 16, tag) == 1;
	if (ok)
		ok = api.EVP_CipherFinal_ex(ctx, (unsigned char *)out + n, &tail) == 1;
	if (ok && encrypt)
		ok = api.EVP_CIPHER_CTX_ctrl(ctx, 0x10 /* GET_TAG */, 16, tag) == 1;
	api.EVP_CIPHER_CTX_free(ctx);
	if (!ok)
		nd_wipe(out, size); // GCM update alone does not authenticate plaintext.
	return ok;
}

bool nd_encrypt(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len, const void *plain, size_t size, uint8_t *cipher, uint8_t tag[16]) { return crypt(true, key, nonce, aad, aad_len, plain, size, cipher, tag); }

bool nd_decrypt(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len, const uint8_t *cipher, size_t size, const uint8_t tag[16], void *plain) {
	uint8_t copy[16];
	memcpy(copy, tag, sizeof(copy));
	bool ok = crypt(false, key, nonce, aad, aad_len, cipher, size, plain, copy);
	nd_wipe(copy, sizeof(copy));
	return ok;
}
