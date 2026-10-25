#ifndef SONIX_GZIP_H
#define SONIX_GZIP_H
#include <stddef.h>
typedef struct gzip_reader gzip_reader_t;
gzip_reader_t *gzip_reader_new(int (*read)(void *, void *, size_t), void *user);
int gzip_reader_read(gzip_reader_t *g, void *out, size_t size);
void gzip_reader_free(gzip_reader_t *g);
size_t gzip_reader_bytes(void);
#endif
