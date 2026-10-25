#include "navidromeapi.h"
#include "navidrome.h"
#include "navidromebatch.h"
#include "navidromedebug.h"
#include "navidromeplayliststore.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *request_url(const char *host, const char *user, const char *password, const char *method, const char *query, char *url, size_t cap) {
	char base[ND_HOST_MAX];
	const char *error = navidrome_validate(host, user, password, base);
	if (error)
		return error;
	uint8_t random[16];
	char salt[33], token[MD5_HEX_LEN], signed_text[ND_SECRET_MAX + 33], encoded[ND_USER_MAX * 3 + 1];
	if (!nd_random(random, sizeof(random)))
		return "nd_crypto_error";
	for (size_t i = 0; i < sizeof(random); i++)
		snprintf(salt + 2 * i, 3, "%02x", random[i]);
	snprintf(signed_text, sizeof(signed_text), "%s%s", password, salt);
	md5_hex(signed_text, strlen(signed_text), token);
	nd_wipe(signed_text, sizeof(signed_text));
	http_url_encode(user, encoded, sizeof(encoded));
	int n = snprintf(url, cap, "%s/rest/%s.view?u=%s&t=%s&s=%s&v=1.16.1&c=SonixPlayer&f=json%s%s", base, method, encoded, token, salt, query && *query ? "&" : "", query ? query : "");
	nd_wipe(token, sizeof(token));
	if (n < 0 || (size_t)n >= cap) {
		nd_wipe(url, cap);
		return "nd_input_long";
	}
	return NULL;
}

const char *nd_api_stream(const char *host, const char *user, const char *password, const char *id, http_stream_t *stream) {
	char encoded[ND_ID_MAX * 3 + 1], query[ND_ID_MAX * 3 + 32], url[2500];
	if (!id || !*id || strlen(id) >= ND_ID_MAX)
		return "nd_response_error";
	http_url_encode(id, encoded, sizeof(encoded));
	snprintf(query, sizeof(query), "id=%s&format=raw", encoded);
	const char *error = request_url(host, user, password, "stream", query, url, sizeof(url));
	if (!error && !http_file_open(stream, url, 8))
		error = stream->status == 401 || stream->status == 403 ? "nd_auth_error" : "nd_network_error";
	nd_wipe(url, sizeof(url));
	http_clear_request_metadata();
	nd_log("navidrome: stream open status=%d accepted=%d\n", stream->status, !error);
	return error;
}

static const char *json_request(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, int tokens, const char *form, nd_document_t *out) {
	memset(out, 0, sizeof(*out));
	char url[2500];
	const char *error = request_url(host, user, password, method, query, url, sizeof(url));
	if (error)
		return error;
	char *body = NULL;
	size_t size = 0, received = 0;
	int status = 0;
	bool too_large = false;
	http_req_t request = {.method = form ? "POST" : NULL, .body = form, .received_out = &received, .diagnostic = nd_debug_line, .body_limit_out = &too_large, .want_error_body = true, .sensitive_url = true, .require_complete_body = true};
	nd_log("navidrome: %s start scheme=%s timeout_seconds=8 body_limit=%zu\n", method, !strncmp(host, "https://", 8) ? "https" : "http", limit);
	bool ok = http_request(url, &request, &body, &size, limit, 8, &status);
	bool diagnostic_error = http_last_error() != NULL;
	nd_log("navidrome: %s response http_ok=%d status=%d bytes=%zu transport_diagnostic=%d\n", method, ok, status, received ? received : size, diagnostic_error);
	http_clear_request_metadata();
	nd_wipe(url, sizeof(url));
	if (!ok) {
		if (body)
			nd_wipe(body, size);
		free(body);
		nd_log("navidrome: %s rejected reason=%s\n", method, status >= 200 && status < 300 ? "incomplete_or_oversized_http_body" : "http_request_failed");
		if (too_large && status >= 200 && status < 300 && strcmp(method, "ping"))
			return tokens == ND_PL_TOKENS ? "nd_pl_too_large" : "nd_page_large";
		if (status >= 200 && status < 300)
			return "nd_response_error";
		if (status >= 400)
			return "nd_api_error";
		return diagnostic_error && !strncmp(host, "https://", 8) ? "nd_tls_error" : "nd_network_error";
	}
	error = "nd_response_error";
	// Only classify response bytes. Never log server text, which may echo
	// credentials, query parameters, cookies, or a login form's hidden inputs.
	size_t first = 0;
	while (first < size && isspace((unsigned char)body[first]))
		first++;
	const char *kind = first == size ? "empty" : body[first] == '{' || body[first] == '[' ? "json" : body[first] == '<' ? "markup" : size - first >= 2 && (unsigned char)body[first] == 0x1f && (unsigned char)body[first + 1] == 0x8b ? "gzip" : "other";
	nd_log("navidrome: %s body kind=%s\n", method, kind);
	const char *reason = !size ? "empty_body" : size >= limit ? "body_limit" : memchr(body, 0, size) ? "embedded_nul" : "malformed_json";
	json_doc_t doc;
	json_parse_result_t parsed = JSON_PARSE_SYNTAX;
	if (size && size < limit && !memchr(body, 0, size))
		parsed = json_parse_detailed(body, &doc, tokens);
	if (parsed == JSON_PARSE_TOKENS) {
		reason = "token_limit";
		error = status >= 400 ? "nd_api_error" : tokens == ND_PL_TOKENS ? "nd_pl_too_large" : strcmp(method, "ping") ? "nd_page_large" : "nd_response_error";
	} else if (parsed == JSON_PARSE_DEPTH) {
		reason = "depth_limit";
		error = "nd_json_depth";
	} else if (parsed == JSON_PARSE_MEMORY) {
		reason = "allocation_failed";
		error = "nd_json_memory";
	} else if (parsed != JSON_PARSE_OK && first < size && (!strncasecmp(body + first, "<html", 5) || !strncasecmp(body + first, "<!doctype html", 14))) {
		reason = "web_page";
		error = "nd_web_page";
	}
	if (parsed == JSON_PARSE_OK) {
		int response = json_get(&doc, json_root(&doc), "subsonic-response");
		char result[32];
		int status_token = json_get(&doc, response, "status");
		bool valid_status = json_str(&doc, status_token, result, sizeof(result));
		reason = response < 0 ? "missing_subsonic_wrapper" : !valid_status ? "missing_or_invalid_status" : "unknown_status";
		if (valid_status) {
			// C strings cannot represent an embedded JSON NUL. In particular,
			// "ok\u0000..." must not authenticate as the exact status "ok".
			for (int i = doc.toks[status_token].start; i + 6 <= doc.toks[status_token].end; i++)
				if (!memcmp(doc.text + i, "\\u0000", 6))
					valid_status = false;
			if (!valid_status)
				reason = "status_contains_nul";
		}
		if (valid_status) {
			if (status >= 200 && status < 300 && !strcmp(result, "ok")) {
				error = NULL;
				reason = "subsonic_ok";
			} else if (!strcmp(result, "failed")) {
				int failure = json_get(&doc, response, "error");
				long code = json_obj_long(&doc, failure, "code", 0);
				error = code == 40 ? "nd_auth_error" : tokens == ND_PL_TOKENS && code == 50 ? "nd_pl_readonly" : tokens == ND_PL_TOKENS && code == 70 ? "nd_pl_missing" : "nd_api_error";
				reason = "subsonic_failed";
				nd_log("navidrome: %s API error_code=%ld\n", method, code);
			}
		}
		if (!error) {
			out->body = body;
			out->doc = doc;
			out->response = response;
			nd_log("navidrome: %s result=connected reason=subsonic_ok\n", method);
			return NULL;
		}
		json_free(&doc);
	}
	nd_log("navidrome: %s result=%s reason=%s bytes=%zu byte_limit=%zu token_limit=%d depth_limit=%d\n", method, error ? error : "connected", reason, size, limit, tokens, JSON_MAX_DEPTH);
	nd_wipe(body, size);
	free(body);
	return error;
}

const char *nd_api_json(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, nd_document_t *out) { return json_request(host, user, password, method, query, limit, ND_API_TOKENS, NULL, out); }

const char *nd_api_playlist(const char *host, const char *user, const char *password, const char *method, const char *query, const char *form, nd_document_t *out) {
	if (form && strlen(form) > ND_PL_BYTES)
		return "nd_pl_too_large";
	return json_request(host, user, password, method, query, ND_PL_BYTES + 1, ND_PL_TOKENS, form, out);
}

void nd_api_free(nd_document_t *doc) {
	json_free(&doc->doc);
	if (doc->body)
		nd_wipe(doc->body, strlen(doc->body));
	free(doc->body);
	memset(doc, 0, sizeof(*doc));
}

const char *nd_api_artwork(const char *host, const char *user, const char *password, const char *id, int pixels, char **body, size_t *size) {
	char encoded[ND_ID_MAX * 3], query[ND_ID_MAX * 3 + 64], url[2500];
	http_url_encode(id, encoded, sizeof(encoded));
	snprintf(query, sizeof(query), "id=%s&size=%d", encoded, pixels);
	const char *error = request_url(host, user, password, "getCoverArt", query, url, sizeof(url));
	if (error)
		return error;
	int status = 0;
	http_req_t request = {.diagnostic = nd_debug_line, .sensitive_url = true, .require_complete_body = true};
	bool ok = http_request(url, &request, body, size, 256 * 1024, 8, &status);
	http_clear_request_metadata();
	nd_wipe(url, sizeof(url));
	if (!ok || status < 200 || status >= 300) {
		if (*body)
			nd_wipe(*body, *size);
		free(*body);
		*body = NULL;
		*size = 0;
		return "nd_response_error";
	}
	return NULL;
}

const char *nd_api_page(http_session_t *session, const char *host, const char *user, const char *password, nd_kind_t kind, int offset, int requested, bool gzip, bool reuse, nd_batch_t *batch) {
	char query[256], url[2500];
	snprintf(query, sizeof(query), "query=&songCount=%d&songOffset=%d&albumCount=%d&albumOffset=%d&artistCount=%d&artistOffset=%d", kind == ND_TRACKS ? requested : 0, offset, kind == ND_ALBUMS ? requested : 0, offset, kind == ND_ARTISTS ? requested : 0, offset);
	bool albums = batch->favorites && kind == ND_ALBUMS;
	if (albums)
		snprintf(query, sizeof(query), "type=starred&size=%d&offset=%d", requested, offset);
	const char *error = request_url(host, user, password, albums ? "getAlbumList2" : kind == ND_GENRES ? "getGenres" : "search3", kind == ND_GENRES ? NULL : query, url, sizeof(url));
	if (!error && !http_session_get(session, url, gzip, reuse)) {
		const char *reason = http_session_error(session);
		error = !strcmp(reason, "body_limit") ? "nd_page_large" : !strcmp(reason, "cancelled") ? "nd_sync_cancelled" : !strcmp(reason, "tls") ? "nd_tls_error" : (!strcmp(reason, "framing") || !strcmp(reason, "encoding_unsupported")) ? "nd_response_error" : !strcmp(reason, "allocation_failed") ? "nd_json_memory" : "nd_network_error";
	}
	nd_wipe(url, sizeof(url));
	http_clear_request_metadata();
	if (!error)
		error = nd_batch_read(batch, session, requested);
	http_session_stats_t stats;
	http_session_stats(session, &stats);
	nd_log("navidrome: sync request kind=%d offset=%d count=%d status=%d encoded_bytes=%zu decoded_bytes=%zu opens=%zu reuses=%zu connect_ms=%.3f transfer_ms=%.3f parse_ms=%.3f buffer_peak=%zu result=%s reason=%s\n", kind, offset, requested, http_session_status(session), stats.encoded, stats.decoded, stats.opens, stats.reuses, stats.connect_seconds * 1000, stats.transfer_seconds * 1000, batch->parse_seconds * 1000, batch->buffer_peak, error ? error : "ok", http_session_error(session) ? http_session_error(session) : "transport_complete");
	return error;
}

bool nd_api_field_unique(const json_doc_t *doc, int object, const char *name, bool required) {
	if (object < 0 || doc->toks[object].type != JSON_OBJECT)
		return false;
	int found = 0;
	for (int t = object + 1; t < doc->count && doc->toks[t].start < doc->toks[object].end; t++) {
		if (doc->toks[t].parent != object)
			continue;
		// json_str() exposes C strings; an escaped NUL must not alias a
		// control name. Skip escaped backslashes so literal text is harmless.
		for (int at = doc->toks[t].start; at < doc->toks[t].end; at++) {
			if (doc->text[at] != '\\')
				continue;
			if (at + 6 <= doc->toks[t].end && !memcmp(doc->text + at, "\\u0000", 6))
				return false;
			at++;
		}
		char key[64];
		if (!json_str(doc, t, key, sizeof(key)))
			return false;
		if (!strcmp(key, name) && ++found > 1)
			return false;
	}
	return found == 1 || (!required && !found);
}

// Scan checks share the finite deadline/cancellation contract, without retaining
// a third connection alongside the two enumeration workers.
static const char *finite_json(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, bool control, bool (*current)(void *), void *arg, nd_document_t *out) {
	memset(out, 0, sizeof(*out));
	http_session_t *session = http_session_new(current, arg);
	if (!session)
		return "nd_json_memory";
	char url[2500];
	const char *error = request_url(host, user, password, method, query, url, sizeof(url));
	if (!error && !http_session_get(session, url, false, false)) {
		const char *reason = http_session_error(session);
		error = !strcmp(reason, "tls") ? "nd_tls_error" : !strcmp(reason, "allocation_failed") ? "nd_json_memory" : "nd_network_error";
	}
	nd_wipe(url, sizeof(url));
	http_clear_request_metadata();
	char *body = !error ? malloc(limit) : NULL;
	size_t used = 0;
	if (!error && !body)
		error = "nd_json_memory";
	while (!error) {
		unsigned char chunk[1024];
		int n = http_session_read(session, chunk, sizeof(chunk));
		if (n < 0) {
			error = "nd_response_error";
			break;
		}
		if (!n)
			break;
		if ((size_t)n >= limit - used || memchr(chunk, 0, n)) {
			error = !control && (size_t)n >= limit - used ? "nd_page_large" : "nd_response_error";
			break;
		}
		memcpy(body + used, chunk, n);
		used += n;
	}
	if (!error) {
		body[used] = 0;
		json_parse_result_t parsed = json_parse_detailed(body, &out->doc, ND_API_TOKENS);
		if (parsed != JSON_PARSE_OK)
			error = parsed == JSON_PARSE_MEMORY ? "nd_json_memory" : !control && parsed == JSON_PARSE_TOKENS ? "nd_page_large" : !control && parsed == JSON_PARSE_DEPTH ? "nd_json_depth" : "nd_response_error";
		else {
			out->response = json_get(&out->doc, json_root(&out->doc), "subsonic-response");
			char status[32];
			int token = json_get(&out->doc, out->response, "status");
			bool valid = nd_api_field_unique(&out->doc, json_root(&out->doc), "subsonic-response", true) && nd_api_field_unique(&out->doc, out->response, "status", true) && json_str(&out->doc, token, status, sizeof(status));
			if (valid) {
				for (int i = out->doc.toks[token].start; i + 6 <= out->doc.toks[token].end; i++)
					if (!memcmp(out->doc.text + i, "\\u0000", 6))
						valid = false;
			}
			if (!valid || strcmp(status, "ok") || http_session_status(session) != 200) {
				int failure = json_get(&out->doc, out->response, "error");
				error = valid && !strcmp(status, "failed") ? json_obj_long(&out->doc, failure, "code", 0) == 40 ? "nd_auth_error" : "nd_api_error" : control || http_session_status(session) >= 400 ? "nd_api_error" : "nd_response_error";
			}
			if (!error && control) {
				int scan = json_get(&out->doc, out->response, "scanStatus");
				if (!nd_api_field_unique(&out->doc, out->response, "scanStatus", true) || !nd_api_field_unique(&out->doc, scan, "scanning", false) || !nd_api_field_unique(&out->doc, scan, "lastScan", false))
					error = "nd_response_error";
			}
			if (error)
				json_free(&out->doc);
		}
	}
	if (http_session_error(session) && !strcmp(http_session_error(session), "cancelled"))
		error = "nd_sync_cancelled";
	http_session_free(session);
	if (error) {
		if (body)
			nd_wipe(body, used);
		free(body);
		memset(out, 0, sizeof(*out));
	} else
		out->body = body;
	return error;
}

const char *nd_api_control(const char *host, const char *user, const char *password, bool (*current)(void *), void *arg, nd_document_t *out) { return finite_json(host, user, password, "getScanStatus", NULL, 16384, true, current, arg, out); }

const char *nd_api_finite_json(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, bool (*current)(void *), void *arg, nd_document_t *out) {
	if (limit < 2 || limit > ND_API_BYTES)
		return "nd_input_long";
	const char *error = finite_json(host, user, password, method, query, limit, false, current, arg, out);
	nd_log("navidrome: finite request method=%s result=%s\n", method, error ? error : "ok");
	return error;
}
