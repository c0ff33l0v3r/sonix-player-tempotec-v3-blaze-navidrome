#ifndef NAVIDROMEAUDIO_H
#define NAVIDROMEAUDIO_H

#include "navidrome.h"

// One blocking worker owns the transfer. ready() may publish a progressive
// path before fetch returns; it must copy inputs and post GUI work itself.
typedef bool (*nd_audio_current_cb)(void *user);
typedef void (*nd_audio_ready_cb)(const char *path, void *user);
const char *nd_audio_fetch(const nd_library_t *library, const nd_context_t *context, const nd_playrow_t *track, bool progressive_start, nd_audio_current_cb current, nd_audio_ready_cb ready, void *user);
void nd_audio_cancel(void);
void nd_audio_protect(const char *const paths[3]);
bool nd_audio_owns(const char *path);
bool nd_audio_network_wanted(void);
void nd_audio_set_network_wanted(bool wanted);
bool nd_audio_failed(const char *path);
bool nd_audio_cached(const nd_library_t *library, const nd_context_t *context, const char *id);

#endif
