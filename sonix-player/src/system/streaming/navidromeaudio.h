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
// Source binding is independent of disposable-file ownership.
bool nd_audio_source(const char *path);
void nd_audio_set_source(const char *path);
typedef bool (*nd_audio_original_resolver)(const nd_library_t *, const nd_context_t *, const char *, char [512]);
void nd_audio_set_original_resolver(nd_audio_original_resolver resolver);
bool nd_audio_network_wanted(void);
void nd_audio_set_network_wanted(bool wanted);
bool nd_audio_failed(const char *path);
bool nd_audio_cached(const nd_library_t *library, const nd_context_t *context, const char *id);
// Original-media validation shared with permanent downloads; worker/card lease.
const char *nd_audio_extension(const unsigned char *bytes, size_t size);
bool nd_audio_valid(const char *path);
bool nd_audio_write_original(int fd, const void *bytes, size_t size); // <=64 KiB, caller owns FD/lease
// One bulk copy owns streamturn before acquiring this reservation.
bool nd_audio_copy_acquire(const nd_library_t *library, const nd_context_t *context, const char *id, char path[512]);
void nd_audio_copy_release(void);

#endif
