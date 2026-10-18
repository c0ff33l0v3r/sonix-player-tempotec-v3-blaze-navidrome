// Host self-check for the DSD -> PCM filter (src/system/decode/dsd.c).
//
// Writes a one-second stereo .dsf holding a 1 kHz sine at half of DSD full
// scale, for each rate from DSD64 to DSD512, plays it through dsd_to_pcm() and
// checks the frequency and the level of what comes out. Also prints how long
// the filter took per second of audio, the figure to scale for the device.
//
//   gcc -O2 -Wall -I. tools/dsd_selftest.c src/system/decode/dsd.c -lpthread -lm -o /tmp/dsd_selftest
//   /tmp/dsd_selftest [dir]      (dir defaults to $TMPDIR or /tmp)

#include "src/system/decode/dsd.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BLOCK 4096
#define TONE_HZ 1000.0
#define AMPLITUDE 0.25 // of the stream's range: half of DSD full scale, so -6 dBFS out

static void put32(unsigned char *p, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		p[i] = (unsigned char)(v >> (8 * i));
	}
}
static void put64(unsigned char *p, uint64_t v) {
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

// A second-order sigma-delta modulator: the bitstream, earliest bit in bit 0.
static void modulate(unsigned char *out, uint64_t bits, uint32_t rate, double phase) {
	double i1 = 0, i2 = 0, y = 0;
	memset(out, 0, (size_t)(bits / 8));
	for (uint64_t n = 0; n < bits; n++) {
		double x = AMPLITUDE * sin(2 * M_PI * TONE_HZ * (double)n / rate + phase);
		i1 += x - y;
		i2 += i1 - y;
		y = i2 >= 0 ? 1.0 : -1.0;
		if (y > 0) {
			out[n / 8] |= (unsigned char)(1u << (n % 8));
		}
	}
}

static void write_dsf(const char *path, uint32_t rate) {
	uint64_t bits = rate; // one second per channel
	uint64_t bytes = bits / 8;
	uint64_t blocks = (bytes + BLOCK - 1) / BLOCK;
	uint64_t data = blocks * BLOCK * 2;

	unsigned char head[92] = {0};
	memcpy(head, "DSD ", 4);
	put64(head + 4, 28);
	put64(head + 12, 92 + data);
	memcpy(head + 28, "fmt ", 4);
	put64(head + 32, 52);
	put32(head + 40, 1);	   // version
	put32(head + 48, 2);	   // channel type: stereo
	put32(head + 52, 2);	   // channels
	put32(head + 56, rate);
	put32(head + 60, 1);	   // bits per sample: LSB first
	put64(head + 64, bits);	   // samples per channel
	put32(head + 72, BLOCK);
	memcpy(head + 80, "data", 4);
	put64(head + 84, 12 + data);

	unsigned char *ch[2];
	for (int c = 0; c < 2; c++) {
		ch[c] = calloc((size_t)(blocks * BLOCK), 1);
		assert(ch[c]);
		modulate(ch[c], bits, rate, c * M_PI / 2); // right a quarter turn behind
	}
	FILE *f = fopen(path, "wb");
	assert(f);
	fwrite(head, 1, sizeof(head), f);
	for (uint64_t b = 0; b < blocks; b++) {
		for (int c = 0; c < 2; c++) {
			fwrite(ch[c] + b * BLOCK, 1, BLOCK, f);
		}
	}
	fclose(f);
	free(ch[0]);
	free(ch[1]);
}

static double seconds_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : (getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	char path[512];
	snprintf(path, sizeof(path), "%s/dsd_selftest.dsf", dir);

	const int multiples[] = {64, 128, 256, 512};
	for (size_t m = 0; m < sizeof(multiples) / sizeof(multiples[0]); m++) {
		uint32_t rate = 44100u * (uint32_t)multiples[m];
		write_dsf(path, rate);

		dsd_file_t *d = dsd_open(path);
		assert(d && dsd_multiple(d) == multiples[m] && dsd_channels(d) == 2);
		assert(dsd_to_pcm(d) && dsd_output_rate(d) == 176400);

		uint64_t total = dsd_total_frames(d);
		int32_t *pcm = malloc((size_t)total * 2 * sizeof(int32_t));
		assert(pcm);
		double t0 = seconds_now();
		uint64_t got = dsd_read(d, total, pcm);
		double took = seconds_now() - t0;
		assert(got == total && total == 176400);

		// Skip the filter's start-up, then count crossings and take the RMS.
		uint64_t from = 4410, crossings = 0;
		double sum = 0;
		for (uint64_t i = from; i < got; i++) {
			double v = pcm[i * 2] / 2147483648.0;
			sum += v * v;
			if ((pcm[i * 2] >= 0) != (pcm[(i - 1) * 2] >= 0)) {
				crossings++;
			}
		}
		double seconds = (double)(got - from) / 176400.0;
		double hz = crossings / 2.0 / seconds;
		double db = 10 * log10(sum / (double)(got - from) * 2); // a sine's peak from its RMS
		printf("DSD%-3d  %.1f Hz  %+.2f dBFS peak  filter %.1f ms per second of audio\n", multiples[m], hz, db,
			   took * 1000);
		assert(fabs(hz - TONE_HZ) < 2.0);
		assert(fabs(db - -6.02) < 0.5);

		free(pcm);
		dsd_close(d);
	}
	unlink(path);
	printf("ok\n");
	return 0;
}
