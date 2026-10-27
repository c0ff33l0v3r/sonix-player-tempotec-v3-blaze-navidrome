// Production representation/getter with an unopened, controlled decoder.
#include "src/system/playback/device_state.c"
#include <assert.h>
static double decoder_position;
static unsigned fixture_identity;
unsigned playlist_identity(void) { return fixture_identity; }
static char decoder_file[512];
static int opens;
audio_status_t audio_get_status(void) { return AUDIO_STATUS_STOPPED; }
void audio_get_current_file(char *out, size_t size) { snprintf(out, size, "%s", decoder_file); }
void audio_get_progress(double *position, double *duration) {
	*position = decoder_position;
	*duration = 300;
}
void audio_get_stream_info(int *rate, int *channels) { *rate = *channels = 0; }
void audio_stop(void) {}
int audio_play(const char *path) {
	(void)path;
	opens++;
	return 0;
}
bool radio_is_active(void) { return false; }
void radio_get_now(radio_now_t *out) { memset(out, 0, sizeof(*out)); }
char *read_battery_percent(void) { return "50"; }
bool read_battery_charging(void) { return false; }
int get_volume_percent(void) { return 20; }
bool nd_audio_source(const char *path) { return path && strstr(path, "/cache/") != NULL; }
bool navidromeplay_reference(const char *path) { return path && !strncmp(path, "ndq:", 4); }
int main(void) {
	song_metadata_t metadata = {.title = "Later occurrence", .has_tags = true};
	device_state_t state;
	device_state_restore_representation("ndq:7:4999", &metadata, 83, 300);
	device_state_get(&state);
	assert(state.status == AUDIO_STATUS_PAUSED && state.progress_current_secs == 83 && opens == 0);
	assert(!strcmp(state.current_file, "ndq:7:4999") && !strcmp(state.metadata.title, "Later occurrence"));
	assert(device_state_resume_position("ndq:7:4999") == 83 && device_state_resume_position("ndq:7:1") == 0);
	// Preparation and failed-result metadata updates retain the paused bookmark.
	device_state_represent_source("ndq:7:4999", &metadata, 300);
	device_state_get(&state);
	assert(state.status == AUDIO_STATUS_PAUSED && state.progress_current_secs == 83);
	// Representation can safely reuse its own loaded-file buffer.
	device_state_restore_representation(current_metadata_file, &metadata, 91, 300);
	device_state_get(&state);
	assert(!strcmp(state.current_file, "ndq:7:4999") && state.progress_current_secs == 91);
	fixture_identity++;
	assert(device_state_resume_position("ndq:7:4999") == 0);
	// The decoder initially reports zero while its requested resume seek lands.
	represented_paused = false;
	strcpy(current_metadata_file, "/cache/track.flac");
	strcpy(decoder_file, current_metadata_file);
	strcpy(pending_resume_file, current_metadata_file);
	pending_resume_position = 91;
	decoder_position = 0;
	device_state_get(&state);
	assert(state.progress_current_secs == 91);
	decoder_position = 91;
	device_state_get(&state);
	assert(state.progress_current_secs == 91 && pending_resume_position < 0);
	decoder_position = 92;
	device_state_get(&state);
	assert(state.progress_current_secs == 92 && opens == 0);
	puts("session position: real paused representation, aliased reference and zero-before-seek bookmark protection passed");
	return 0;
}
