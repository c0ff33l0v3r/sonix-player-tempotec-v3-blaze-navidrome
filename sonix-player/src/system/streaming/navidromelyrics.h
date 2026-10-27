#ifndef NAVIDROMELYRICS_H
#define NAVIDROMELYRICS_H
#include "navidrome.h"
#include "navidromeapi.h"
#include "src/system/library/lrclib.h"
#include "src/system/library/lyrics.h"
#define ND_LYRIC_TEXT (256 * 1024)
#define ND_LYRIC_LINES 4000
#define ND_LYRIC_VERSIONS 8

typedef struct {
	lyrics_t lyrics;
	char language[40];
} nd_lyric_doc_t;
typedef struct {
	nd_lyric_doc_t versions[ND_LYRIC_VERSIONS];
	int count, selected;
	bool override, unsaved;
} nd_lyric_set_t;
typedef struct {
	nd_context_t context; // lifetime fields only, never credentials
	nd_library_t library;
	char id[ND_ID_MAX], reference[1024];
	char title[256], artist[256], album[256];
	int duration;
	unsigned queue;
} nd_lyric_target_t;
typedef enum { ND_LYRIC_LOCAL, ND_LYRIC_VIEW, ND_LYRIC_REFRESH, ND_LYRIC_SEARCH, ND_LYRIC_APPLY, ND_LYRIC_SERVER, ND_LYRIC_SELECT } nd_lyric_action_t;
typedef struct {
	nd_lyric_target_t target;
	nd_lyric_action_t action;
	nd_lyric_set_t set;
	lrclib_record_t *records;
	int count;
	const char *error;
} nd_lyric_result_t;
// Callback borrows result; moving owned lyrics/records requires clearing the fields.
typedef void (*nd_lyric_done_cb)(nd_lyric_result_t *result, void *user);
bool nd_lyrics_target(const char *reference, nd_lyric_target_t *out);
bool nd_lyrics_same(const nd_lyric_target_t *a, const nd_lyric_target_t *b);
unsigned nd_lyrics_submit(const nd_lyric_target_t *target, nd_lyric_action_t action, const char *text, int choice, nd_lyric_done_cb done, void *user);
void nd_lyrics_cancel(unsigned token);
void nd_lyrics_poll(void);	   // GUI fallback delivery
void nd_lyrics_shutdown(void); // process teardown, never on navigation
bool nd_lyrics_copy(const lyrics_t *source, lyrics_t *out);
char *nd_lyrics_text(const lyrics_t *source);
void nd_lyrics_free(nd_lyric_set_t *set);
const char *nd_lyrics_decode(const nd_document_t *document, nd_lyric_set_t *out);
// Worker-only store boundary, also used by native fixtures. Returns NULL on success.
const char *nd_lyrics_store(const char *path, const char *scope, const char *id, nd_lyric_action_t action, nd_lyric_set_t *set, const char *text, int choice, bool (*current)(void *), void *user);
bool nd_lyrics_path(const nd_library_t *library, char out[1200]);
#endif
