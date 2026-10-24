#include "navidromeapi.h"
#include "navidrome.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

const char *nd_api_json(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, nd_document_t *out) {
	memset(out, 0, sizeof(*out));
	char url[2500];
	const char *error = request_url(host, user, password, method, query, url, sizeof(url));
	if (error)
		return error;
	char *body = NULL;
	size_t size = 0;
	int status = 0;
	bool too_large = false;
	http_req_t request = {.body_limit_out = &too_large, .want_error_body = true, .sensitive_url = true, .require_complete_body = true};
	fprintf(stderr, "navidrome: %s start scheme=%s timeout_seconds=8 body_limit=%zu\n", method, !strncmp(host, "https://", 8) ? "https" : "http", limit);
	bool ok = http_request(url, &request, &body, &size, limit, 8, &status);
	bool diagnostic_error = http_last_error() != NULL;
	fprintf(stderr, "navidrome: %s response http_ok=%d status=%d bytes=%zu transport_diagnostic=%d\n", method, ok, status, size, diagnostic_error);
	http_clear_request_metadata();
	nd_wipe(url, sizeof(url));
	if (!ok) {
		if (body)
			nd_wipe(body, size);
		free(body);
		fprintf(stderr, "navidrome: %s rejected reason=%s\n", method, status >= 200 && status < 300 ? "incomplete_or_oversized_http_body" : "http_request_failed");
		if (too_large && strcmp(method, "ping"))
			return "nd_page_large";
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
	fprintf(stderr, "navidrome: %s body kind=%s\n", method, kind);
	const char *reason = !size ? "empty_body" : size >= limit ? "body_limit" : memchr(body, 0, size) ? "embedded_nul" : "malformed_json";
	json_doc_t doc;
	if (size && size < limit && !memchr(body, 0, size) && json_parse_limited(body, &doc, ND_API_TOKENS)) {
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
				error = code == 40 ? "nd_auth_error" : "nd_api_error";
				reason = "subsonic_failed";
				fprintf(stderr, "navidrome: %s API error_code=%ld\n", method, code);
			}
		}
		if (!error) {
			out->body = body;
			out->doc = doc;
			out->response = response;
			fprintf(stderr, "navidrome: %s result=connected reason=subsonic_ok\n", method);
			return NULL;
		}
		json_free(&doc);
	}
	fprintf(stderr, "navidrome: %s result=%s reason=%s\n", method, error ? error : "connected", reason);
	nd_wipe(body, size);
	free(body);
	return error;
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
	http_req_t request = {.sensitive_url = true, .require_complete_body = true};
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
