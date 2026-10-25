#include "navidromeaudio.h"
#include "navidromelibrary.h"
#include "src/system/core/md5.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/growfile.h"
#include "streamturn.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define AUDIO_BUDGET (1024LL * 1024 * 1024)
#define AUDIO_RESERVE (16 * 1024 * 1024)
#define AUDIO_CHUNK (64 * 1024)
typedef struct {
	uint32_t version;
	char scope[33], id[ND_ID_MAX], extension[8];
	int64_t bytes;
} complete_t;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *active;
static unsigned cancelled;
static bool network_wanted;
static char protected_paths[3][512];
static char failed_path[512];
static pthread_once_t once = PTHREAD_ONCE_INIT;
static bool registered;

void nd_audio_cancel(void) {
	pthread_mutex_lock(&lock);
	cancelled++;
	if (active)
		http_stream_wake(active);
	pthread_mutex_unlock(&lock);
}
static void register_cache(void) { registered = streamturn_register_abandon(nd_audio_cancel); }
bool nd_audio_failed(const char *path) {
	pthread_mutex_lock(&lock);
	bool failed = path && path[0] && !strcmp(path, failed_path);
	pthread_mutex_unlock(&lock);
	return failed;
}
bool nd_audio_owns(const char *path) { return path && (strstr(path, "/" ND_CACHE_RELATIVE "/audio/") || strstr(path, "/.local/navidrome/audio/")); }
bool nd_audio_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool wanted = network_wanted;
	pthread_mutex_unlock(&lock);
	return wanted;
}
void nd_audio_set_network_wanted(bool wanted) {
	pthread_mutex_lock(&lock);
	network_wanted = wanted;
	pthread_mutex_unlock(&lock);
}
void nd_audio_protect(const char *const paths[3]) {
	pthread_mutex_lock(&lock);
	for (int i = 0; i < 3; i++)
		snprintf(protected_paths[i], sizeof(protected_paths[i]), "%s", paths && paths[i] ? paths[i] : "");
	pthread_mutex_unlock(&lock);
}
static bool protected(const char *path) {
	bool keep = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < 3; i++)
		keep |= protected_paths[i][0] && !strcmp(path, protected_paths[i]);
	pthread_mutex_unlock(&lock);
	return keep || growfile_is_growing(path);
}
static bool directory(const char *path) {
	struct stat st;
	if (mkdir(path, 0700) && errno != EEXIST)
		return false;
	return !lstat(path, &st) && S_ISDIR(st.st_mode);
}
static bool names(const nd_library_t *lib, const char *id, char root[512], char stem[512]) {
	char base[1024], input[ND_ID_MAX + 100], key[33];
	snprintf(base, sizeof(base), "%s", lib->path);
	char *slash = strrchr(base, '/');
	if (!slash)
		return false;
	*slash = 0;
	if (snprintf(root, 512, "%s/audio", base) >= 512)
		return false;
	snprintf(input, sizeof(input), "%s:%zu:%s:raw:v1", lib->scope, strlen(id), id);
	md5_hex(input, strlen(input), key);
	return snprintf(stem, 512, "%s/%s/%s", root, lib->scope, key) < 490;
}
static bool hex32(const char *name) { return strlen(name) == 32 && strspn(name, "0123456789abcdef") == 32; }
static bool suffix(char *out, size_t cap, const char *stem, const char *ending) {
	size_t a = strlen(stem), b = strlen(ending);
	if (a + b >= cap)
		return false;
	memcpy(out, stem, a);
	memcpy(out + a, ending, b + 1);
	return true;
}
static bool owned_file(const char *name) { return strlen(name) > 33 && strspn(name, "0123456789abcdef") == 32 && name[32] == '.' && (!strcmp(name + 32, ".complete") || !strcmp(name + 32, ".tmp") || !strcmp(name + 32, ".flac") || !strcmp(name + 32, ".mp3") || !strcmp(name + 32, ".m4a") || !strcmp(name + 32, ".ogg") || !strcmp(name + 32, ".opus") || !strcmp(name + 32, ".wav") || !strcmp(name + 32, ".aiff") || !strcmp(name + 32, ".caf") || !strcmp(name + 32, ".aac") || !strcmp(name + 32, ".dsf") || !strcmp(name + 32, ".dff") || !strcmp(name + 32, ".wv") || !strcmp(name + 32, ".ape")); }
static bool completed_file(const char *path) {
	char marker[512];
	snprintf(marker, sizeof(marker), "%s", path);
	char *dot = strrchr(marker, '.');
	if (!dot || (size_t)(dot - marker) + sizeof(".complete") > sizeof(marker))
		return false;
	strcpy(dot, ".complete");
	complete_t record;
	struct stat st;
	int fd = open(marker, O_RDONLY | O_NOFOLLOW);
	bool ok = fd >= 0 && !fstat(fd, &st) && st.st_size == sizeof(record) && read(fd, &record, sizeof(record)) == sizeof(record);
	if (fd >= 0)
		close(fd);
	if (!ok || record.version != 1 || !memchr(record.extension, 0, sizeof(record.extension)) || !strcmp(record.extension, ".complete") || !strcmp(record.extension, ".tmp") || record.bytes <= 0)
		return false;
	strcpy(dot, record.extension);
	return owned_file(strrchr(marker, '/') + 1) && !lstat(marker, &st) && S_ISREG(st.st_mode) && st.st_size == record.bytes;
}
// Called only under a short card lease. Budget includes every owned scope.
static bool room(const char *root, int64_t incoming) {
	for (;;) {
		DIR *scopes = opendir(root);
		if (!scopes)
			return false;
		int64_t bytes = 0;
		time_t oldest = 0;
		char victim[512] = "";
		struct dirent *scope;
		while ((scope = readdir(scopes))) {
			if (!hex32(scope->d_name))
				continue;
			char dir[512];
			struct stat ds;
			if (snprintf(dir, sizeof(dir), "%s/%s", root, scope->d_name) >= (int)sizeof(dir) || lstat(dir, &ds) || !S_ISDIR(ds.st_mode))
				continue;
			DIR *files = opendir(dir);
			if (!files)
				continue;
			struct dirent *file;
			while ((file = readdir(files))) {
				if (!owned_file(file->d_name))
					continue;
				char path[512];
				struct stat st;
				if (snprintf(path, sizeof(path), "%s/%s", dir, file->d_name) >= (int)sizeof(path) || lstat(path, &st) || !S_ISREG(st.st_mode))
					continue;
				if (!strcmp(file->d_name + 32, ".tmp")) {
					unlink(path);
					continue;
				}
				if (!protected(path) && !completed_file(path)) {
					unlink(path); // abandoned partial, never reusable as complete audio
					continue;
				}
				bytes += st.st_size;
				if (strstr(file->d_name, ".complete") || strstr(file->d_name, ".tmp") || protected(path))
					continue;
				if (!victim[0] || st.st_mtime < oldest) {
					oldest = st.st_mtime;
					snprintf(victim, sizeof(victim), "%s", path);
				}
			}
			closedir(files);
		}
		closedir(scopes);
		struct statvfs fs;
		bool free_space = !statvfs(root, &fs) && (uint64_t)fs.f_bavail * fs.f_frsize >= (uint64_t)(incoming + AUDIO_RESERVE);
		if (bytes + incoming <= AUDIO_BUDGET && free_space)
			return true;
		if (!victim[0])
			return false;
		char marker[512];
		snprintf(marker, sizeof(marker), "%s", victim);
		char *dot = strrchr(marker, '.');
		if (!dot)
			return false;
		strcpy(dot, ".complete");
		if (unlink(victim))
			return false;
		unlink(marker);
	}
}
static const char *extension(const unsigned char *p, size_t n) {
	if (n < 12)
		return NULL;
	if (!memcmp(p, "fLaC", 4))
		return ".flac";
	if (!memcmp(p, "ID3", 3))
		return ".mp3";
	if (p[0] == 0xff && (p[1] & 0xe0) == 0xe0)
		return (p[1] & 0xf6) == 0xf0 ? ".aac" : ".mp3";
	if (!memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WAVE", 4))
		return ".wav";
	if (!memcmp(p, "FORM", 4))
		return ".aiff";
	if (!memcmp(p, "caff", 4))
		return ".caf";
	if (!memcmp(p, "OggS", 4)) {
		for (size_t i = 0; i + 8 <= n; i++)
			if (!memcmp(p + i, "OpusHead", 8))
				return ".opus";
		return ".ogg";
	}
	if (!memcmp(p + 4, "ftyp", 4))
		return ".m4a";
	if (!memcmp(p, "DSD ", 4))
		return ".dsf";
	if (!memcmp(p, "FRM8", 4))
		return ".dff";
	if (!memcmp(p, "wvpk", 4))
		return ".wv";
	if (!memcmp(p, "MAC ", 4))
		return ".ape";
	return NULL;
}
static bool cached(const nd_library_t *lib, const char *stem, const char *id, char path[512]) {
	char marker[512];
	complete_t record = {0};
	struct stat st;
	if (snprintf(marker, sizeof(marker), "%s.complete", stem) >= (int)sizeof(marker))
		return false;
	int fd = open(marker, O_RDONLY | O_NOFOLLOW);
	bool ok = fd >= 0 && !fstat(fd, &st) && st.st_size == sizeof(record) && read(fd, &record, sizeof(record)) == sizeof(record);
	if (fd >= 0)
		close(fd);
	if (!ok || record.version != 1 || !memchr(record.scope, 0, sizeof(record.scope)) || !memchr(record.id, 0, sizeof(record.id)) || !memchr(record.extension, 0, sizeof(record.extension)) || strcmp(record.scope, lib->scope) || strcmp(record.id, id) || record.bytes <= 0 || record.bytes > AUDIO_BUDGET)
		return false;
	if (snprintf(path, 512, "%s%s", stem, record.extension) >= 512 || !owned_file(strrchr(path, '/') + 1))
		return false;
	return !nd_audio_failed(path) && !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_size == record.bytes;
}

static uint32_t little32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static bool valid_audio(const char *path) {
	decode_format_t format = decode_detect_format(path);
	if (format != DECODE_FORMAT_UNKNOWN) {
		decoder_t *decoder = decoder_open_info(path, format);
		bool valid = decoder && decoder_sample_rate(decoder) > 0 && decoder_channels(decoder) > 0;
		if (decoder)
			decoder_close(decoder);
		return valid;
	}
	// WAV uses audio.c's native reader rather than a decoder. Check its finite
	// RIFF structure without making libsndfile a new requirement for WAV.
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	unsigned char header[16];
	struct stat st;
	bool valid = !fstat(fileno(f), &st) && fread(header, 1, 12, f) == 12 && !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVE", 4) && (int64_t)little32(header + 4) + 8 <= st.st_size;
	bool fmt = false, data = false;
	for (int i = 0; valid && i < 128 && !data; i++) {
		if (fread(header, 1, 8, f) != 8)
			break;
		uint32_t size = little32(header + 4);
		off_t next = ftello(f) + size + (size & 1);
		if (next > st.st_size) {
			valid = false;
			break;
		}
		if (!memcmp(header, "fmt ", 4)) {
			fmt = size >= 16 && fread(header, 1, 16, f) == 16 && header[2] && little32(header + 4);
		} else if (!memcmp(header, "data", 4))
			data = size > 0;
		if (fseeko(f, next, SEEK_SET)) {
			valid = false;
			break;
		}
	}
	fclose(f);
	return valid && fmt && data;
}
typedef struct {
	const nd_context_t *context;
	nd_audio_current_cb current;
	void *user;
	unsigned cancellation;
} turn_context_t;
static bool turn_current(void *user) {
	turn_context_t *wait = user;
	pthread_mutex_lock(&lock);
	bool valid = wait->cancellation == cancelled;
	pthread_mutex_unlock(&lock);
	return valid && wait->current(wait->user) && navidrome_playback_current(wait->context, true);
}

bool nd_audio_cached(const nd_library_t *lib, const nd_context_t *ctx, const char *id) {
	char root[512], stem[512], path[512];
	if (!names(lib, id, root, stem) || !navidrome_playback_begin(ctx))
		return false;
	bool hit = cached(lib, stem, id, path);
	navidrome_cache_end();
	return hit;
}

const char *nd_audio_fetch(const nd_library_t *lib, const nd_context_t *ctx, const nd_playrow_t *track, bool progressive_start, nd_audio_current_cb current, nd_audio_ready_cb ready, void *user) {
	pthread_once(&once, register_cache);
	if (!registered)
		return "nd_worker_error";
	char root[512], stem[512], path[512], marker[512], temp[512];
	if (!names(lib, track->row.id, root, stem) || !navidrome_playback_begin(ctx))
		return "nd_cache_storage";
	bool hit = cached(lib, stem, track->row.id, path);
	if (hit && !valid_audio(path)) {
		pthread_mutex_lock(&lock);
		snprintf(failed_path, sizeof(failed_path), "%s", path);
		pthread_mutex_unlock(&lock);
		if (lib->writable && suffix(marker, sizeof(marker), stem, ".complete"))
			unlink(marker);
		navidrome_cache_end();
		return "nd_audio_invalid";
	}
	if (hit && lib->writable)
		utime(path, NULL);
	navidrome_cache_end();
	if (hit) {
		if (current(user))
			ready(path, user);
		return NULL;
	}
	if (!navidrome_playback_current(ctx, true))
		return "nd_reconnect_required";
	if (!lib->writable)
		return "nd_cache_readonly";
	pthread_mutex_lock(&lock);
	unsigned generation = cancelled;
	pthread_mutex_unlock(&lock);
	turn_context_t wait = {.context = ctx, .current = current, .user = user, .cancellation = generation};
	if (!streamturn_acquire_current(turn_current, &wait))
		return current(user) && !navidrome_playback_current(ctx, true) ? "nd_network_error" : "nd_audio_cancelled";
	http_stream_t stream = {.fd = -1};
	const char *error = turn_current(&wait) ? nd_api_stream(ctx->host, ctx->user, ctx->password, track->row.id, &stream) : "nd_audio_cancelled";
	if (!error && (strstr(stream.content_type, "text/") || strstr(stream.content_type, "json") || strstr(stream.content_type, "xml")))
		error = "nd_audio_invalid";
	if (!error && stream.length_known && (stream.content_length <= 0 || stream.content_length > AUDIO_BUDGET))
		error = "nd_audio_space";
	pthread_mutex_lock(&lock);
	if (!error)
		active = &stream;
	pthread_mutex_unlock(&lock);
	unsigned char *buffer = malloc(AUDIO_CHUNK);
	if (!buffer && !error)
		error = "nd_worker_error";
	int fd = -1;
	long done = 0;
	bool announced = false, delivered = false, progressive = false;
	path[0] = 0;
	long prebuffer = STREAM_PREBUFFER;
	struct timespec probe_start;
	clock_gettime(CLOCK_MONOTONIC, &probe_start);
	bool measured_probe = false;
	if (stream.content_length > 0 && track->row.duration > 0) {
		double seconds_floor = (double)stream.content_length / track->row.duration * STREAM_HEAD_SECS;
		long floor = seconds_floor > stream.content_length ? stream.content_length : (long)seconds_floor;
		if (floor > prebuffer)
			prebuffer = floor;
	}
	while (!error) {
		pthread_mutex_lock(&lock);
		bool cancelled_now = generation != cancelled;
		pthread_mutex_unlock(&lock);
		if (!current(user)) {
			error = "nd_audio_cancelled";
			break;
		}
		if (!navidrome_playback_current(ctx, true)) {
			error = "nd_network_error";
			break;
		}
		if (cancelled_now) {
			error = "nd_audio_cancelled";
			break;
		}
		int n = http_stream_read(&stream, buffer, AUDIO_CHUNK);
		if (!path[0] && n > 0) {
			while (n < 64) {
				int more = http_stream_read(&stream, buffer + n, AUDIO_CHUNK - n);
				if (more < 0) {
					n = -1;
					break;
				}
				if (!more)
					break;
				n += more;
			}
		}
		// A wake can return either an error or EOF. Recheck intent before
		// interpreting either result as a network failure or a complete file.
		pthread_mutex_lock(&lock);
		cancelled_now = generation != cancelled;
		pthread_mutex_unlock(&lock);
		if (!current(user)) {
			error = "nd_audio_cancelled";
			break;
		}
		if (!navidrome_playback_current(ctx, true)) {
			error = "nd_network_error";
			break;
		}
		if (cancelled_now) {
			error = "nd_audio_cancelled";
			break;
		}
		if (n < 0) {
			error = "nd_network_error";
			break;
		}
		if (!n)
			break;
		if (!navidrome_playback_begin(ctx)) {
			error = "nd_audio_cancelled";
			break;
		}
		if (!path[0]) {
			const char *ext = extension(buffer, (size_t)n);
			if (!ext)
				error = "nd_audio_invalid";
			else if (snprintf(path, sizeof(path), "%s%s", stem, ext) >= (int)sizeof(path))
				error = "nd_cache_storage";
			else {
				progressive = !strcmp(ext, ".mp3") || !strcmp(ext, ".flac");
				char dir[512];
				if (snprintf(dir, sizeof(dir), "%s/%s", root, lib->scope) >= (int)sizeof(dir))
					error = "nd_cache_storage";
				if (!directory(root) || !directory(dir) || protected(path) || !room(root, (int64_t)n + sizeof(complete_t)))
					error = "nd_audio_space";
				else {
					suffix(marker, sizeof(marker), stem, ".complete");
					unlink(marker); // a replaced partial file cannot retain an old completion record
					fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
					if (fd >= 0) {
						growfile_announce(path, stream.length_known ? stream.content_length : 0);
						announced = true;
					}
				}
				if (!error && fd < 0)
					error = "nd_cache_storage";
			}
		} else {
			fd = open(path, O_WRONLY | O_APPEND | O_NOFOLLOW);
			if (fd < 0)
				error = "nd_cache_storage";
		}
		if (!error && !room(root, (int64_t)n + sizeof(complete_t)))
			error = "nd_audio_space";
		if (!error) {
			size_t at = 0;
			while (at < (size_t)n) {
				ssize_t wrote = write(fd, buffer + at, (size_t)n - at);
				if (wrote <= 0) {
					error = "nd_cache_storage";
					break;
				}
				at += (size_t)wrote;
			}
		}
		if (!error) {
			done += n;
			if (announced)
				growfile_progress(path, done);
		}
		if (fd >= 0) {
			if (close(fd) && !error)
				error = "nd_cache_storage";
			fd = -1;
		}
		navidrome_cache_end();
		if (!measured_probe && done >= STREAM_PROBE) {
			measured_probe = true;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - probe_start.tv_sec) + (now.tv_nsec - probe_start.tv_nsec) / 1e9;
			if (elapsed > 0.3 && stream.length_known && track->row.duration > 0) {
				double required = (double)stream.content_length / track->row.duration;
				double head = stream.content_length * (1.0 - (done / elapsed) / (required * 1.2));
				double cap = required * STREAM_HEAD_MAX_SECS;
				if (head > cap)
					head = cap;
				if (head > prebuffer)
					prebuffer = (long)head;
				if (prebuffer > stream.content_length)
					prebuffer = stream.content_length;
			}
		}
		if (!error && progressive_start && progressive && !delivered && done >= prebuffer && current(user)) {
			ready(path, user);
			delivered = true;
		}
	}
	pthread_mutex_lock(&lock);
	if (active == &stream)
		active = NULL;
	pthread_mutex_unlock(&lock);
	http_stream_close(&stream);
	free(buffer);
	if (fd >= 0 && close(fd) && !error)
		error = "nd_cache_storage";
	if (!error && !done)
		error = "nd_audio_invalid";
	if (!error && (!current(user) || !navidrome_playback_current(ctx, true)))
		error = "nd_audio_cancelled";
	// A completed writer must stop advertising growth before info-only MP3
	// validation, which may otherwise wait for bytes beyond its final frame.
	if (error && path[0]) {
		pthread_mutex_lock(&lock);
		snprintf(failed_path, sizeof(failed_path), "%s", path);
		pthread_mutex_unlock(&lock);
	}
	if (announced) {
		growfile_finish(path, !error);
		announced = false;
	}
	if (!error && navidrome_playback_begin(ctx)) {
		if (!valid_audio(path))
			error = "nd_audio_invalid";
		if (!error) {
			int data = open(path, O_WRONLY | O_NOFOLLOW);
			if (data < 0 || fsync(data))
				error = "nd_cache_storage";
			if (data >= 0 && close(data))
				error = "nd_cache_storage";
		}
		if (!error) {
			complete_t record = {.version = 1, .bytes = done};
			snprintf(record.scope, sizeof(record.scope), "%s", lib->scope);
			snprintf(record.id, sizeof(record.id), "%s", track->row.id);
			snprintf(record.extension, sizeof(record.extension), "%s", strrchr(path, '.'));
			suffix(marker, sizeof(marker), stem, ".complete");
			suffix(temp, sizeof(temp), stem, ".tmp");
			int out = open(temp, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
			bool ok = out >= 0 && write(out, &record, sizeof(record)) == sizeof(record) && !fsync(out);
			if (out >= 0 && close(out))
				ok = false;
			if (!ok || !navidrome_playback_publish(ctx, temp, marker))
				error = "nd_cache_storage";
			unlink(temp);
		}
		navidrome_cache_end();
	} else if (!error)
		error = "nd_audio_cancelled";
	pthread_mutex_lock(&lock);
	if (path[0] && (error || !strcmp(path, failed_path)))
		snprintf(failed_path, sizeof(failed_path), "%s", error ? path : "");
	pthread_mutex_unlock(&lock);
	if (!error && !delivered && current(user))
		ready(path, user);
	if (error && (!strcmp(error, "nd_network_error") || !strcmp(error, "nd_auth_error")))
		navidrome_network_failed(ctx);
	streamturn_release();
	return error;
}
