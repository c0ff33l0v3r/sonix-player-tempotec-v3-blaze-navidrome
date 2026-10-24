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
void library_sort_key_ex(const char *name, char *out, size_t size, bool skip_articles) {
	(void)skip_articles;
	size_t i;
	for (i = 0; name[i] && i + 1 < size; i++)
		out[i] = (char)tolower((unsigned char)name[i]);
	out[i] = 0;
}
