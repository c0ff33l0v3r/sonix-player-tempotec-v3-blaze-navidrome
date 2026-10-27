#include "navidromelyrics.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nd_lyrics_free(nd_lyric_set_t *s) {
	for (int i = 0; i < ND_LYRIC_VERSIONS; i++)
		lyrics_free(&s->versions[i].lyrics);
	memset(s, 0, sizeof(*s));
}
static bool integer(const json_doc_t *d, int token, int64_t *out) {
	if (token < 0 || d->toks[token].type != JSON_NUMBER)
		return false;
	int n = d->toks[token].end - d->toks[token].start;
	if (n < 1 || n > 20)
		return false;
	char buf[24], *end;
	memcpy(buf, d->text + d->toks[token].start, n);
	buf[n] = 0;
	errno = 0;
	long long v = strtoll(buf, &end, 10);
	if (errno || *end)
		return false;
	*out = v;
	return true;
}
// Decode into a maximum raw-size buffer so json_str cannot silently truncate.
static char *string(const json_doc_t *d, int token) {
	if (token < 0 || d->toks[token].type != JSON_STRING)
		return NULL;
	for (int at = d->toks[token].start; at < d->toks[token].end; at++) {
		if (d->text[at] != '\\')
			continue;
		if (at + 6 <= d->toks[token].end && !memcmp(d->text + at, "\\u0000", 6))
			return NULL;
		at++;
	}
	size_t n = d->toks[token].end - d->toks[token].start;
	char *s = malloc(n + 1);
	if (!s || !json_str(d, token, s, n + 1)) {
		free(s);
		return NULL;
	}
	// Escaped NUL is forbidden by the shared strict-field check at its parent.
	return s;
}
const char *nd_lyrics_decode(const nd_document_t *doc, nd_lyric_set_t *out) {
	memset(out, 0, sizeof(*out));
	const json_doc_t *d = &doc->doc;
	int list = json_get(d, doc->response, "lyricsList");
	int array = json_get(d, list, "structuredLyrics");
	if (!nd_api_field_unique(d, doc->response, "lyricsList", true) || !nd_api_field_unique(d, list, "structuredLyrics", false))
		return "nd_response_error";
	// Navidrome omits its empty slice, returning lyricsList: {} for a miss.
	if (array < 0)
		return "lyrics_not_found";
	if (d->toks[array].type != JSON_ARRAY)
		return "nd_response_error";
	int count = json_len(d, array);
	if (count > ND_LYRIC_VERSIONS)
		return "nd_lyrics_limit";
	size_t aggregate = 0;
	const char *error = "nd_response_error";
	for (int i = 0; i < count; i++) {
		int item = json_at(d, array, i), lines = json_get(d, item, "line");
		const char *fields[] = {"synced", "line", "lang", "offset"};
		for (int f = 0; f < 4; f++)
			if (!nd_api_field_unique(d, item, fields[f], f < 2))
				goto fail;
		int sync = json_get(d, item, "synced");
		if (sync < 0 || d->toks[sync].type != JSON_BOOL || lines < 0 || d->toks[lines].type != JSON_ARRAY)
			goto fail;
		int n = json_len(d, lines);
		if (n > ND_LYRIC_LINES) {
			error = "nd_lyrics_limit";
			goto fail;
		}
		if (!n)
			continue;
		nd_lyric_doc_t *v = &out->versions[out->count];
		lyrics_t *l = &v->lyrics;
		l->synced = json_bool(d, sync, false);
		int64_t offset = 0;
		int off = json_get(d, item, "offset");
		if (off >= 0 && (!integer(d, off, &offset) || offset < INT32_MIN || offset > INT32_MAX))
			goto fail;
		int lang = json_get(d, item, "lang");
		if (lang >= 0) {
			char *name = string(d, lang);
			if (!name || strlen(name) >= sizeof(v->language)) {
				free(name);
				goto fail;
			}
			strcpy(v->language, name);
			free(name);
		}
		l->lines = calloc(n, sizeof(*l->lines));
		l->arena = malloc(ND_LYRIC_TEXT + 1);
		if (!l->lines || !l->arena) {
			error = "nd_json_memory";
			goto fail;
		}
		size_t used = 0;
		int32_t previous = 0;
		bool words = false;
		for (int j = 0; j < n; j++) {
			int line = json_at(d, lines, j);
			if (!nd_api_field_unique(d, line, "value", true) || !nd_api_field_unique(d, line, "start", l->synced))
				goto fail;
			int64_t time = -1;
			if (l->synced) {
				if (!integer(d, json_get(d, line, "start"), &time) || time < 0 || time > INT32_MAX || time - offset > INT32_MAX)
					goto fail;
				time -= offset;
				if (time < 0)
					time = 0;
				if (j && time < previous)
					goto fail;
				previous = (int32_t)time;
			}
			char *value = string(d, json_get(d, line, "value"));
			if (!value)
				goto fail;
			size_t size = strlen(value) + 1;
			if (size > ND_LYRIC_TEXT - used || size > 1024 * 1024 - aggregate) {
				free(value);
				error = "nd_lyrics_limit";
				goto fail;
			}
			words |= size > 1;
			memcpy(l->arena + used, value, size);
			free(value);
			l->lines[j] = (lyrics_line_t){.ms = (int32_t)time, .text = l->arena + used};
			used += size;
			aggregate += size;
		}
		l->count = n;
		if (!words) {
			lyrics_free(l);
			continue;
		}
		out->count++;
	}
	if (!out->count)
		return "lyrics_not_found";
	// Stable partition, keeping each server group in order.
	for (int i = 1; i < out->count; i++)
		if (out->versions[i].lyrics.synced) {
			nd_lyric_doc_t v = out->versions[i];
			int j = i;
			while (j && !out->versions[j - 1].lyrics.synced) {
				out->versions[j] = out->versions[j - 1];
				j--;
			}
			out->versions[j] = v;
		}
	return NULL;
fail:
	nd_lyrics_free(out);
	return error;
}

bool nd_lyrics_copy(const lyrics_t *s, lyrics_t *out) {
	memset(out, 0, sizeof(*out));
	if (s->count < 1 || s->count > ND_LYRIC_LINES)
		return false;
	size_t total = 0;
	for (int i = 0; i < s->count; i++) {
		size_t n = strlen(s->lines[i].text) + 1;
		if (n > ND_LYRIC_TEXT - total)
			return false;
		total += n;
	}
	out->arena = malloc(total);
	out->lines = calloc(s->count, sizeof(*out->lines));
	if (!out->arena || !out->lines) {
		lyrics_free(out);
		return false;
	}
	size_t at = 0;
	for (int i = 0; i < s->count; i++) {
		size_t n = strlen(s->lines[i].text) + 1;
		memcpy(out->arena + at, s->lines[i].text, n);
		out->lines[i] = (lyrics_line_t){.ms = s->lines[i].ms, .text = out->arena + at};
		at += n;
	}
	out->count = s->count;
	out->synced = s->synced;
	return true;
}
char *nd_lyrics_text(const lyrics_t *s) {
	size_t size = 1;
	for (int i = 0; i < s->count; i++)
		size += strlen(s->lines[i].text) + 32;
	char *out = malloc(size);
	if (!out)
		return NULL;
	size_t used = 0;
	for (int i = 0; i < s->count; i++) {
		int32_t ms = s->lines[i].ms;
		if (s->synced)
			used += (size_t)snprintf(out + used, size - used, "[%02d:%02d.%03d]", ms / 60000, (ms / 1000) % 60, ms % 1000);
		used += (size_t)snprintf(out + used, size - used, "%s\n", s->lines[i].text);
	}
	out[used] = 0;
	return out;
}
