#include "src/system/core/json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_allocation;
void *__real_realloc(void *, size_t);
void *__wrap_realloc(void *p, size_t size) {
	if (fail_allocation)
		return NULL;
	return __real_realloc(p, size);
}

int main(void) {
	json_doc_t doc;
	assert(json_parse_detailed("{", &doc, 100) == JSON_PARSE_SYNTAX);
	char *body = malloc(40002);
	assert(body);
	body[0] = '[';
	for (int i = 0; i < 20000; i++) {
		body[1 + 2 * i] = '0';
		body[2 + 2 * i] = ',';
	}
	body[40000] = ']';
	body[40001] = 0;
	assert(json_parse_detailed(body, &doc, 16384) == JSON_PARSE_TOKENS);
	assert(json_parse_detailed(body, &doc, 24000) == JSON_PARSE_OK);
	assert(doc.count == 20001);
	json_free(&doc);
	free(body);
	char deep[131];
	memset(deep, '[', 65);
	memset(deep + 65, ']', 65);
	deep[130] = 0;
	assert(json_parse_detailed(deep, &doc, 1000) == JSON_PARSE_DEPTH);
	fail_allocation = 1;
	assert(json_parse_detailed("{}", &doc, 100) == JSON_PARSE_MEMORY);
	fail_allocation = 0;
	assert(json_parse("{}", &doc));
	json_free(&doc);
	puts("JSON syntax/token/depth/allocation distinction and boolean compatibility: passed");
}
