#ifndef SESSIONMUSIC_H
#define SESSIONMUSIC_H
#include "playlist.h"
typedef struct {
	library_index_spec_t query;
	char **paths;
	int *slots;
	int count, current;
	bool custom;
	char album[256], selected[512];
	double position;
} session_music_t;
session_music_t *session_music_capture(const char *selected, double position);
void session_music_free(session_music_t *music);
bool session_music_write(const session_header_t *header, const session_music_t *music, const char *root);
session_music_t *session_music_read(const session_header_t *header, const char *root);
void session_music_prune(const session_header_t *keep, const char *root);
#endif
