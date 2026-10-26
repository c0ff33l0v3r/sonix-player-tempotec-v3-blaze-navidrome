// The focused fixtures contain ASCII names. Production registers the shared
// library collation; this stand-in avoids linking the entire local music index.
#include <strings.h>
int library_collate_listorder(void *unused, int na, const void *a, int nb, const void *b) {
	(void)unused;
	int n = na < nb ? na : nb;
	int result = strncasecmp(a, b, n);
	return result ? result : (na > nb) - (na < nb);
}

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
__attribute__((weak)) const char *tr(const char *key) { return key; }
__attribute__((weak)) const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}
void library_sort_key_ex(const char *name, char *out, size_t size, bool skip_articles) {
	(void)skip_articles;
	size_t i;
	for (i = 0; name[i] && i + 1 < size; i++)
		out[i] = (char)tolower((unsigned char)name[i]);
	out[i] = 0;
}

char library_index_letter_of_key(const char *key) {
	if (!key || !*key)
		return '#';
	unsigned char c = (unsigned char)*key;
	return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c >= 128 ? '~' : '#';
}
int library_index_slot(char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 1 : c == '~' ? 27 : 0; }

#include <string.h>
size_t library_fold_text(const char *text, char *out, size_t size) {
	size_t n = 0;
	if (!size) return 0;
	if (text) for (; text[n] && n + 1 < size; n++) out[n] = (char)tolower((unsigned char)text[n]);
	out[n] = 0;
	return n;
}
bool library_search_pattern(const char *text, char *out, size_t size) {
	char folded[256];
	if (size < 3 || !library_fold_text(text, folded, sizeof(folded))) return false;
	size_t n = 0; out[n++] = '%';
	for (size_t i = 0; folded[i] && n + 3 < size; i++) {
		if (strchr("%_\\", folded[i])) out[n++] = '\\';
		out[n++] = folded[i];
	}
	out[n++] = '%'; out[n] = 0;
	return true;
}

// Presentation-only configuration for focused UI fixtures; production config
// persistence is exercised separately with the real config module.
static int fixture_album_layout = -1;
__attribute__((weak)) bool config_get_bool(const char *section, const char *key, bool fallback) {
    if (!strcmp(section, "navidrome") && !strcmp(key, "album_head") && fixture_album_layout >= 0)
        return fixture_album_layout != 0;
    return fallback;
}
__attribute__((weak)) void config_set_bool(const char *section, const char *key, bool value) {
    if (!strcmp(section, "navidrome") && !strcmp(key, "album_head")) fixture_album_layout = value;
}
__attribute__((weak)) bool config_save(void) { return true; }

extern bool nd_audio_owns(const char *) __attribute__((weak));
__attribute__((weak)) bool nd_audio_source(const char *path) { return (path && !strncmp(path, "ndq:", 4)) || (nd_audio_owns && nd_audio_owns(path)); }
