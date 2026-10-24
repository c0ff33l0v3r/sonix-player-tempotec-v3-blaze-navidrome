#define _GNU_SOURCE 1
#include "navidromevault.h"
#include "src/system/core/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECORD_MAX 8192

bool nd_vault_path(const char *config, char *out, size_t size) {
	if (!config || !config[0]) {
#ifdef HOST_BUILD
		// Host settings otherwise live only in memory. Navidrome still needs
		// an encrypted record before connecting; keep it in the launch directory.
		int n = snprintf(out, size, "%s", ND_VAULT_FILE);
		return n > 0 && (size_t)n < size;
#else
		return false;
#endif
	}
	const char *slash = strrchr(config, '/');
	int n = slash ? snprintf(out, size, "%.*s/%s", (int)(slash - config), config, ND_VAULT_FILE) : snprintf(out, size, "%s", ND_VAULT_FILE);
	return n > 0 && (size_t)n < size;
}

static void hex(const uint8_t *in, size_t n, char *out) {
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * n] = 0;
}

static bool unhex(const char *in, uint8_t *out, size_t n) {
	if (strlen(in) != 2 * n)
		return false;
	for (size_t i = 0; i < 2 * n; i++) {
		char c = in[i];
		int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
		if (v < 0)
			return false;
		if (!(i & 1))
			out[i / 2] = (uint8_t)(v << 4);
		else
			out[i / 2] |= (uint8_t)v;
	}
	return true;
}

// Decode into a file-sized scratch buffer: json_str intentionally truncates
// for catalogue callers. Vault metadata must never be silently truncated.
static bool string(const json_doc_t *doc, int root, const char *name, char *out, size_t size) {
	int t = json_get(doc, root, name);
	if (t < 0 || doc->toks[t].type != JSON_STRING)
		return false;
	const json_tok_t *token = &doc->toks[t];
	for (int i = token->start; i < token->end; i++) {
		if (doc->text[i] == '\\') {
			i++;
			if (i + 4 < token->end && !memcmp(doc->text + i, "u0000", 5))
				return false;
		}
	}
	char decoded[RECORD_MAX];
	bool ok = json_str(doc, t, decoded, sizeof(decoded)) && strlen(decoded) < size;
	if (ok)
		memcpy(out, decoded, strlen(decoded) + 1);
	nd_wipe(decoded, sizeof(decoded));
	return ok;
}

static unsigned integer(const json_doc_t *doc, int root, const char *name) {
	int t = json_get(doc, root, name);
	if (t < 0 || doc->toks[t].type != JSON_NUMBER)
		return 0;
	unsigned value = 0;
	for (int i = doc->toks[t].start; i < doc->toks[t].end; i++) {
		char c = doc->text[i];
		if (c < '0' || c > '9' || value > ND_KDF_MAX_ITERATIONS)
			return 0;
		value = value * 10 + (unsigned)(c - '0');
	}
	return value;
}

int nd_vault_load(const char *path, nd_record_t *r) {
	memset(r, 0, sizeof(*r));
	if (!path || !*path)
		return -1;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return errno == ENOENT ? 0 : -1;
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size >= RECORD_MAX) {
		close(fd);
		return -1;
	}
	FILE *f = fdopen(fd, "r");
	if (!f) {
		close(fd);
		return -1;
	}
	char text[RECORD_MAX], value[ND_SECRET_MAX * 2 + 1];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	bool ok = !ferror(f) && n == (size_t)st.st_size && !memchr(text, 0, n);
	text[n] = 0;
	fclose(f);
	json_doc_t doc;
	if (!ok || !json_parse(text, &doc)) {
		nd_wipe(text, sizeof(text));
		return -1;
	}
	int root = json_root(&doc);
	unsigned count = integer(&doc, root, "iterations");
	int history = json_get(&doc, root, "authenticated");
	r->version = integer(&doc, root, "version");
	bool mode_ok = r->version == 1 || (r->version == 2 && string(&doc, root, "mode", value, sizeof(value)) && (!strcmp(value, "plain") || !strcmp(value, "encrypted")));
	r->plain = r->version == 2 && mode_ok && !strcmp(value, "plain");
	ok = root >= 0 && doc.toks[root].type == JSON_OBJECT && mode_ok && (r->version == 1 || r->version == 2) && json_len(&doc, root) == (r->plain ? 6 : r->version == 1 ? 10 : 11) && history >= 0 && doc.toks[history].type == JSON_BOOL && string(&doc, root, "host", r->host, sizeof(r->host)) && r->host[0] && string(&doc, root, "username", r->user, sizeof(r->user)) && r->user[0];
	if (ok && r->plain) {
		ok = string(&doc, root, "password", (char *)r->cipher, sizeof(r->cipher)) && r->cipher[0];
		r->size = strlen((char *)r->cipher);
	} else if (ok) {
		ok = count >= ND_KDF_ITERATIONS && count <= ND_KDF_MAX_ITERATIONS && string(&doc, root, "kdf", value, sizeof(value)) && !strcmp(value, "pbkdf2-sha256") && string(&doc, root, "salt", value, sizeof(value)) && unhex(value, r->salt, sizeof(r->salt)) && string(&doc, root, "nonce", value, sizeof(value)) && unhex(value, r->nonce, sizeof(r->nonce)) && string(&doc, root, "tag", value, sizeof(value)) && unhex(value, r->tag, sizeof(r->tag)) && string(&doc, root, "ciphertext", value, sizeof(value));
		if (ok) {
			r->size = strlen(value) / 2;
			ok = r->size > 0 && r->size < ND_SECRET_MAX && unhex(value, r->cipher, r->size);
			r->iterations = count;
		}
	}
	if (ok) {
		r->authenticated = json_bool(&doc, history, false);
	}
	json_free(&doc);
	nd_wipe(text, sizeof(text));
	nd_wipe(value, sizeof(value));
	if (!ok)
		nd_wipe(r, sizeof(*r));
	return ok ? 1 : -1;
}

bool nd_passphrase_valid(const char *text) {
	if (!text || strlen(text) >= ND_SECRET_MAX)
		return false;
	size_t count = 0;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++)
		if ((*p & 0xc0) != 0x80)
			count++;
	return count >= ND_UNLOCK_PASSWORD_MIN_CHARS;
}

bool nd_vault_prepare(nd_record_t *r, const char *passphrase, uint8_t key[32]) {
	r->version = 2;
	r->plain = false;
	nd_wipe(r->cipher, sizeof(r->cipher));
	r->iterations = ND_KDF_ITERATIONS;
	return nd_passphrase_valid(passphrase) && nd_random(r->salt, sizeof(r->salt)) && nd_derive(passphrase, r->salt, r->iterations, key);
}

static size_t associated(const nd_record_t *r, char *out, size_t size) {
	char salt[33];
	hex(r->salt, 16, salt);
	const char *prefix = r->version == 1 ? "sonix-navidrome-v1" : "sonix-navidrome-v2|encrypted";
	int n = snprintf(out, size, "%s|pbkdf2-sha256|%u|%s|%zu:%s|%zu:%s|%d", prefix, r->iterations, salt, strlen(r->host), r->host, strlen(r->user), r->user, r->authenticated);
	return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

bool nd_vault_seal(nd_record_t *r, const uint8_t key[32], const char *password) {
	if (r->plain)
		return nd_vault_plain(r, password);
	if (!password || !*password || strlen(password) >= ND_SECRET_MAX)
		return false;
	char aad[ND_HOST_MAX + ND_USER_MAX + 160];
	size_t n = associated(r, aad, sizeof(aad));
	r->size = strlen(password);
	return n && nd_random(r->nonce, sizeof(r->nonce)) && nd_encrypt(key, r->nonce, aad, n, password, r->size, r->cipher, r->tag);
}

bool nd_vault_open(const nd_record_t *r, const uint8_t key[32], char password[ND_SECRET_MAX]) {
	if (r->plain) {
		nd_wipe(password, ND_SECRET_MAX);
		if (!r->size || r->size >= ND_SECRET_MAX || strnlen((const char *)r->cipher, sizeof(r->cipher)) != r->size)
			return false;
		memcpy(password, r->cipher, r->size);
		return true;
	}
	char aad[ND_HOST_MAX + ND_USER_MAX + 160];
	size_t n = associated(r, aad, sizeof(aad));
	nd_wipe(password, ND_SECRET_MAX);
	if (!n || !r->size || r->size >= ND_SECRET_MAX || !nd_decrypt(key, r->nonce, aad, n, r->cipher, r->size, r->tag, password))
		return false;
	if (memchr(password, 0, r->size)) {
		nd_wipe(password, ND_SECRET_MAX);
		return false;
	}
	password[r->size] = 0;
	return true;
}

bool nd_vault_plain(nd_record_t *r, const char *password) {
	if (!password || !*password || strlen(password) >= ND_SECRET_MAX)
		return false;
	nd_wipe(r->salt, sizeof(r->salt));
	nd_wipe(r->nonce, sizeof(r->nonce));
	nd_wipe(r->tag, sizeof(r->tag));
	nd_wipe(r->cipher, sizeof(r->cipher));
	r->version = 2;
	r->plain = true;
	r->iterations = 0;
	r->size = strlen(password);
	memcpy(r->cipher, password, r->size);
	return true;
}

static void quoted(FILE *f, const char *s) {
	fputc('"', f);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			fputc('\\', f);
			fputc(*p, f);
		} else if (*p < 32)
			fprintf(f, "\\u%04x", *p);
		else
			fputc(*p, f);
	}
	fputc('"', f);
}

bool nd_vault_stage(const char *path, const nd_record_t *r) {
	char tmp[1024], value[ND_SECRET_MAX * 2 + 1];
	if (!r->size || r->size >= ND_SECRET_MAX || (r->version != 1 && r->version != 2) || (r->plain && (r->version != 2 || strnlen((const char *)r->cipher, sizeof(r->cipher)) != r->size)) || (!r->plain && (r->iterations < ND_KDF_ITERATIONS || r->iterations > ND_KDF_MAX_ITERATIONS)) || strnlen(r->host, sizeof(r->host)) >= sizeof(r->host) || strnlen(r->user, sizeof(r->user)) >= sizeof(r->user))
		return 0;
	if (!path || !*path || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return 0;
	if (unlink(tmp) && errno != ENOENT)
		return 0;
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0)
		return 0;
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		return 0;
	}
	fprintf(f, "{\n\"version\":%u,", r->version);
	if (r->version == 2)
		fprintf(f, "\n\"mode\":\"%s\",", r->plain ? "plain" : "encrypted");
	fputs("\n\"host\":", f);
	quoted(f, r->host);
	fputs(",\n\"username\":", f);
	quoted(f, r->user);
	fprintf(f, ",\n\"authenticated\":%s,", r->authenticated ? "true" : "false");
	if (r->plain) {
		fputs("\n\"password\":", f);
		quoted(f, (const char *)r->cipher);
		fputs("\n}\n", f);
	} else {
		fprintf(f, "\n\"kdf\":\"pbkdf2-sha256\",\n\"iterations\":%u,", r->iterations);
		hex(r->salt, 16, value);
		fprintf(f, "\n\"salt\":\"%s\",", value);
		hex(r->nonce, 12, value);
		fprintf(f, "\n\"nonce\":\"%s\",", value);
		hex(r->tag, 16, value);
		fprintf(f, "\n\"tag\":\"%s\",", value);
		hex(r->cipher, r->size, value);
		fprintf(f, "\n\"ciphertext\":\"%s\"\n}\n", value);
	}
	nd_wipe(value, sizeof(value));
	bool ok = !ferror(f) && fflush(f) == 0 && fsync(fd) == 0;
	if (fclose(f))
		ok = false;
	if (!ok) {
		unlink(tmp);
		return false;
	}
	return true;
}

bool nd_vault_commit(const char *path) {
	char tmp[1024];
	return path && *path && snprintf(tmp, sizeof(tmp), "%s.tmp", path) < (int)sizeof(tmp) && rename(tmp, path) == 0;
}

void nd_vault_discard(const char *path) {
	char tmp[1024];
	if (path && *path && snprintf(tmp, sizeof(tmp), "%s.tmp", path) < (int)sizeof(tmp))
		unlink(tmp);
}

bool nd_vault_sync(const char *path) {
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (!slash)
		snprintf(dir, sizeof(dir), ".");
	else if (slash == dir)
		slash[1] = 0;
	else
		*slash = 0;
	int fd = open(dir, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		return false;
	bool ok = fsync(fd) == 0 || errno == EINVAL || errno == EROFS;
	close(fd);
	return ok;
}

int nd_vault_save(const char *path, const nd_record_t *r) {
	if (!nd_vault_stage(path, r))
		return 0;
	if (!nd_vault_commit(path)) {
		nd_vault_discard(path);
		return 0;
	}
	return nd_vault_sync(path) ? 1 : 2;
}

bool nd_vault_remove(const char *path) {
	char tmp[1024];
	if (!path || !*path || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return false;
	bool ok = unlink(tmp) == 0 || errno == ENOENT;
	if (unlink(path) && errno != ENOENT)
		ok = false;
	return ok;
}
