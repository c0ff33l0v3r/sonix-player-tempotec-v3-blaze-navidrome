#include "src/system/library/library.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int sign(int value) { return (value > 0) - (value < 0); }

int main(void) {
	const char *names[] = {"", "!Live", "007", "alpha", "Alpha", "The Beatles", "Beatles", "An Album", "il Disco", "Étoile", "Ωμέγα", "Москва", "あいう", "中文", "한국어", "Zebra"};
	for (int skip = 0; skip < 2; skip++) {
		library_set_skip_articles(skip);
		for (unsigned a = 0; a < sizeof(names) / sizeof(names[0]); a++) {
			char ka[LIBRARY_SORT_KEY_MAX], legacy[LIBRARY_SORT_KEY_MAX];
			library_sort_key_ex(names[a], ka, sizeof(ka), skip);
			library_sort_key(names[a], legacy, sizeof(legacy));
			assert(!strcmp(ka, legacy));
			for (unsigned b = 0; b < sizeof(names) / sizeof(names[0]); b++) {
				char kb[LIBRARY_SORT_KEY_MAX];
				library_sort_key_ex(names[b], kb, sizeof(kb), skip);
				assert(sign(strcmp(ka, kb)) == sign(library_collate_listorder(NULL, strlen(names[a]), names[a], strlen(names[b]), names[b])));
			}
		}
	}
	char before[LIBRARY_SORT_KEY_MAX], after[LIBRARY_SORT_KEY_MAX];
	library_set_skip_articles(true);
	library_sort_key_ex("The Beatles", before, sizeof(before), true);
	library_set_skip_articles(false);
	library_sort_key_ex("The Beatles", after, sizeof(after), true);
	assert(!strcmp(before, after));
	puts("Shared Unicode ordering, legacy callers, and fixed snapshot policy: passed");
	return 0;
}
