#include "navidromebatch.h"
#include "navidromedebug.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Pull parsing validates every byte, including ignored subtrees. Only one entity
// is retained raw; complete envelope/HTTP validation precedes any SQLite import.
typedef struct {
	http_session_t *session;
	nd_batch_t *batch;
	unsigned char input[8192];
	size_t pos, end, raw_size;
	char *raw;
	bool capture;
	int depth, entity_tokens, controls, requested, code;
	unsigned seen;
	char status[32];
	const char *reason;
} page_parser_t;
enum { ROOT, WRAPPER, RESULT, ENTITIES, ENTITY, OTHER, FAILURE };
enum { SEEN_WRAPPER = 1, SEEN_STATUS = 2, SEEN_RESULT = 4, SEEN_ARRAY = 8, SEEN_ERROR = 16 };
static double page_now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static bool fail(page_parser_t *p, const char *error, const char *reason) {
	if (!p->batch->error) {
		p->batch->error = error;
		p->reason = reason;
	}
	return false;
}
static int peek(page_parser_t *p) {
	if (p->batch->error)
		return -1;
	if (p->pos == p->end) {
		int n = http_session_read(p->session, p->input, sizeof(p->input));
		if (n <= 0)
			return -1;
		p->pos = 0;
		p->end = n;
	}
	return p->input[p->pos];
}
static int take(page_parser_t *p) {
	int c = peek(p);
	if (c < 0)
		return -1;
	p->pos++;
	if (p->capture) {
		if (p->raw_size + 1 >= ND_API_BYTES) {
			fail(p, "nd_page_large", "entity_bytes");
			return -1;
		}
		p->raw[p->raw_size++] = c;
	}
	return c;
}
static void space(page_parser_t *p) {
	int c;
	while ((c = peek(p)) == ' ' || c == '\t' || c == '\r' || c == '\n')
		take(p);
}
static bool token(page_parser_t *p) {
	int *n = p->capture ? &p->entity_tokens : &p->controls;
	return ++*n <= ND_API_TOKENS || fail(p, "nd_page_large", "token_limit");
}
static int hex(int c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}
static bool string(page_parser_t *p, char *out, size_t size) {
	char encoded[512];
	size_t used = 0;
	if (take(p) != '"')
		return false;
	encoded[used++] = '"';
	for (;;) {
		int c = take(p);
		if (c < 32)
			return false;
		if (used + 1 < sizeof(encoded))
			encoded[used] = c;
		used++;
		if (c == '"')
			break;
		if (c == '\\') {
			c = take(p);
			if (used + 1 < sizeof(encoded))
				encoded[used] = c;
			used++;
			if (c < 0 || !strchr("\"\\/bfnrtu", c))
				return false;
			if (c == 'u') {
				unsigned code = 0;
				for (int i = 0; i < 4; i++) {
					c = take(p);
					int digit = hex(c);
					if (digit < 0)
						return false;
					code = code * 16 + digit;
					if (used + 1 < sizeof(encoded))
						encoded[used] = c;
					used++;
				}
				if (out && !code)
					return false;
				if (code >= 0xdc00 && code <= 0xdfff)
					return false;
				if (code >= 0xd800 && code <= 0xdbff) {
					for (int i = 0; i < 2; i++) {
						c = take(p);
						if (c != (i ? 'u' : '\\'))
							return false;
						if (used + 1 < sizeof(encoded))
							encoded[used] = c;
						used++;
					}
					unsigned low = 0;
					for (int i = 0; i < 4; i++) {
						c = take(p);
						int digit = hex(c);
						if (digit < 0)
							return false;
						low = low * 16 + digit;
						if (used + 1 < sizeof(encoded))
							encoded[used] = c;
						used++;
					}
					if (low < 0xdc00 || low > 0xdfff)
						return false;
				}
			}
		} else if (c >= 128) {
			int extra = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
			if (extra < 0)
				return false;
			unsigned code = c & ((1u << (6 - extra)) - 1);
			unsigned minimum = extra == 1 ? 128 : extra == 2 ? 2048 : 65536;
			for (int i = 0; i < extra; i++) {
				c = take(p);
				if (c < 128 || c > 191)
					return false;
				code = code * 64 + (c & 63);
				if (used + 1 < sizeof(encoded))
					encoded[used] = c;
				used++;
			}
			if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
				return false;
		}
	}
	if (out) {
		out[0] = 1;
		out[1] = 0;
		if (used + 1 < sizeof(encoded)) {
			encoded[used] = 0;
			json_doc_t d;
			if (!json_parse_limited(encoded, &d, 4))
				return false;
			bool ok = json_str(&d, json_root(&d), out, size);
			json_free(&d);
			if (!ok)
				return false;
		}
	}
	return true;
}
static bool value(page_parser_t *p, int role);
static bool once(page_parser_t *p, unsigned bit) {
	if (p->seen & bit)
		return false;
	p->seen |= bit;
	return true;
}
static bool object(page_parser_t *p, int role) {
	if (take(p) != '{')
		return false;
	space(p);
	if (peek(p) == '}') {
		take(p);
		return true;
	}
	for (;;) {
		char key[64];
		if (!token(p) || !string(p, key, sizeof(key)))
			return false;
		space(p);
		if (take(p) != ':')
			return false;
		space(p);
		int child = OTHER;
		if (role == ROOT && !strcmp(key, "subsonic-response")) {
			if (!once(p, SEEN_WRAPPER))
				return false;
			child = WRAPPER;
		} else if (role == WRAPPER && !strcmp(key, "status")) {
			if (!once(p, SEEN_STATUS) || !token(p) || !string(p, p->status, sizeof(p->status)))
				return false;
			goto member_done;
		} else if (role == WRAPPER && !strcmp(key, p->batch->kind == ND_GENRES ? "genres" : "searchResult3")) {
			if (!once(p, SEEN_RESULT))
				return false;
			child = RESULT;
		} else if (role == WRAPPER && !strcmp(key, "error")) {
			if (!once(p, SEEN_ERROR))
				return false;
			child = FAILURE;
		} else if (role == RESULT) {
			static const char *names[] = {"album", "song", "artist", "genre"};
			if (!strcmp(key, names[p->batch->kind])) {
				if (!once(p, SEEN_ARRAY))
					return false;
				child = ENTITIES;
			}
		} else if (role == FAILURE && !strcmp(key, "code")) {
			// Capture only a bounded integer code; never arbitrary server text.
			char text[16];
			size_t used = 0;
			while (peek(p) >= '0' && peek(p) <= '9') {
				if (used + 1 >= sizeof(text))
					return false;
				text[used++] = take(p);
			}
			if (!used)
				return false;
			if (used > 1 && text[0] == '0')
				return false;
			text[used] = 0;
			long code = strtol(text, NULL, 10);
			if (code < 0 || code > 2147483647L || (p->seen & 32))
				return false;
			p->seen |= 32;
			p->code = code;
			if (!token(p))
				return false;
			goto member_done;
		}
		if (!value(p, child))
			return false;
	member_done:
		space(p);
		int c = take(p);
		if (c == '}')
			return true;
		if (c != ',')
			return false;
		space(p);
	}
}
static bool array(page_parser_t *p, int role) {
	if (take(p) != '[')
		return false;
	space(p);
	if (peek(p) == ']') {
		take(p);
		return true;
	}
	for (;;) {
		if (role == ENTITIES && p->batch->kind != ND_GENRES && p->batch->count >= p->requested)
			return fail(p, "nd_sync_inconsistent", "invalid_count");
		if (!value(p, role == ENTITIES ? ENTITY : OTHER))
			return false;
		space(p);
		int c = take(p);
		if (c == ']')
			return true;
		if (c != ',')
			return false;
		space(p);
	}
}
static bool number(page_parser_t *p) {
	if (peek(p) == '-')
		take(p);
	if (peek(p) == '0')
		take(p);
	else {
		if (peek(p) < '1' || peek(p) > '9')
			return false;
		do {
			take(p);
		} while (peek(p) >= '0' && peek(p) <= '9');
	}
	if (peek(p) == '.') {
		take(p);
		if (peek(p) < '0' || peek(p) > '9')
			return false;
		do {
			take(p);
		} while (peek(p) >= '0' && peek(p) <= '9');
	}
	if (peek(p) == 'e' || peek(p) == 'E') {
		take(p);
		if (peek(p) == '+' || peek(p) == '-')
			take(p);
		if (peek(p) < '0' || peek(p) > '9')
			return false;
		do {
			take(p);
		} while (peek(p) >= '0' && peek(p) <= '9');
	}
	return true;
}
static bool value(page_parser_t *p, int role) {
	space(p);
	if (++p->depth > JSON_MAX_DEPTH)
		return fail(p, "nd_json_depth", "depth_limit");
	bool entity = role == ENTITY;
	if (entity) {
		p->capture = true;
		p->raw_size = 0;
		p->entity_tokens = 0;
	}
	if (!token(p))
		return false;
	int c = peek(p);
	bool ok = false;
	if ((role == ROOT || role == WRAPPER || role == RESULT || role == FAILURE || entity) && c != '{')
		return false;
	if (role == ENTITIES && c != '[')
		return false;
	if (c == '{')
		ok = object(p, role);
	else if (c == '[')
		ok = array(p, role);
	else if (c == '"')
		ok = string(p, NULL, 0);
	else if (c == '-' || (c >= '0' && c <= '9'))
		ok = number(p);
	else {
		const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : "";
		ok = *literal != 0;
		for (; ok && *literal; literal++)
			ok = take(p) == *literal;
	}
	if (ok && entity) {
		p->raw[p->raw_size] = 0;
		json_doc_t doc;
		size_t token_peak = 0;
		json_parse_result_t result = json_parse_detailed_peak(p->raw, &doc, ND_API_TOKENS, &token_peak);
		size_t peak = ND_BATCH_ARENA + ND_API_BYTES + sizeof(*p) + http_session_bytes() + token_peak;
		if (peak > p->batch->buffer_peak)
			p->batch->buffer_peak = peak;
		if (result != JSON_PARSE_OK)
			return fail(p, result == JSON_PARSE_MEMORY ? "nd_json_memory" : result == JSON_PARSE_TOKENS ? "nd_page_large" : "nd_response_error", result == JSON_PARSE_TOKENS ? "token_limit" : "entity_json");
		ok = nd_batch_add(p->batch, &doc, json_root(&doc));
		json_free(&doc);
		if (!ok && !p->batch->error)
			p->batch->error = "nd_sync_inconsistent";
		p->capture = false;
	}
	p->depth--;
	return ok;
}
const char *nd_batch_read(nd_batch_t *b, http_session_t *session, int requested) {
	page_parser_t *p = calloc(1, sizeof(*p));
	if (!p)
		return b->error = "nd_json_memory";
	p->session = session;
	p->batch = b;
	p->requested = requested;
	p->raw = malloc(ND_API_BYTES);
	if (!p->raw) {
		free(p);
		return b->error = "nd_json_memory";
	}
	// Includes worst realloc overlap for the detailed entity token parser.
	b->buffer_peak = ND_BATCH_ARENA + ND_API_BYTES + sizeof(*p) + http_session_bytes();
	if (b->buffer_peak + 3 * ND_API_TOKENS * sizeof(json_tok_t) / 2 > ND_BATCH_SLOT_BYTES) {
		free(p->raw);
		free(p);
		return b->error = "nd_json_memory";
	}
	double start = page_now();
	space(p);
	bool markup = peek(p) == '<';
	bool ok = !markup && value(p, ROOT);
	space(p);
	if (ok)
		ok = peek(p) < 0;
	const char *transport = http_session_error(session);
	if (transport) {
		b->error = !strcmp(transport, "body_limit") || !strcmp(transport, "decoded_limit") ? "nd_page_large" : !strcmp(transport, "cancelled") ? "nd_sync_cancelled" : !strcmp(transport, "allocation_failed") ? "nd_json_memory" : "nd_response_error";
		p->reason = transport;
	} else if (!ok && !b->error)
		b->error = markup ? "nd_web_page" : "nd_response_error";
	if (!b->error && ((p->seen & (SEEN_WRAPPER | SEEN_STATUS)) != (SEEN_WRAPPER | SEEN_STATUS)))
		b->error = "nd_response_error";
	if (!b->error && !strcmp(p->status, "failed"))
		b->error = p->code == 40 ? "nd_auth_error" : "nd_api_error";
	if (!b->error && (strcmp(p->status, "ok") || !(p->seen & SEEN_RESULT)))
		b->error = "nd_response_error";
	if (!b->error && (http_session_status(session) < 200 || http_session_status(session) >= 300))
		b->error = "nd_api_error";
	http_session_stats_t stats;
	http_session_stats(session, &stats);
	b->parse_seconds = page_now() - start - stats.transfer_seconds;
	if (b->parse_seconds < 0)
		b->parse_seconds = 0;
	if (b->error)
		nd_log("navidrome: sync parse kind=%d reason=%s result=%s encoded_bytes=%zu decoded_bytes=%zu\n", b->kind, p->reason ? p->reason : "invalid_envelope_or_entity", b->error, stats.encoded, stats.decoded);
	free(p->raw);
	free(p);
	return b->error;
}
