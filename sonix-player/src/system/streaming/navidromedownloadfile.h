#ifndef NAVIDROMEDOWNLOADFILE_H
#define NAVIDROMEDOWNLOADFILE_H

#include <stdbool.h>

typedef enum {
	ND_DOWNLOAD_PUBLISHED,
	ND_DOWNLOAD_PUBLISH_CONFLICT,
	ND_DOWNLOAD_PUBLISH_UNSUPPORTED,
	ND_DOWNLOAD_PUBLISH_ERROR,
} nd_download_publish_result_t;

// Worker-only publication primitive. The caller owns a short card lease and a
// pinned destination directory fd; part/final are basenames within it. The
// partial must already be validated, flushed and closed, with durable job intent.
// Never substitutes a replacing rename on unsupported kernels/filesystems.
// Success does not itself commit an association or promise power-loss durability.
nd_download_publish_result_t nd_download_file_publish(int directory, const char *part, const char *final);

// Compatibility primitives, not a complete publication operation. The caller
// MUST persist independent visibility protection before exclusive creation and
// retain it until full validation and durable completion. Never reopen a final
// pathname for writing: keep this owned descriptor for the entire copy.
int nd_download_file_create(int directory, const char *final);
bool nd_download_file_owned(int directory, const char *final, int fd);

#endif
