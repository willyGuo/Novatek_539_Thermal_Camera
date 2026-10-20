#include "timed_snapshot.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	unsigned int min;
	unsigned int max;
	double mean;
	double stddev;
} RAW_STATS;

static RAW_STATS timed_snapshot_stats(const unsigned char *raw, size_t pixels)
{
	RAW_STATS stats = {65535U, 0U, 0.0, 0.0};
	uint64_t sum = 0;
	double squared_deviation = 0.0;
	size_t i;

	/* Read the saved little-endian UINT16 values without masking RAW DN. */
	for (i = 0; i < pixels; i++) {
		unsigned int value = raw[2 * i] | ((unsigned int)raw[2 * i + 1] << 8);
		if (value < stats.min) stats.min = value;
		if (value > stats.max) stats.max = value;
		sum += value;
	}
	stats.mean = (double)sum / (double)pixels;
	for (i = 0; i < pixels; i++) {
		unsigned int value = raw[2 * i] | ((unsigned int)raw[2 * i + 1] << 8);
		double delta = (double)value - stats.mean;
		squared_deviation += delta * delta;
	}
	/* Population standard deviation over every pixel, including outliers. */
	stats.stddev = sqrt(squared_deviation / (double)pixels);
	return stats;
}

static int timed_snapshot_close_csv(FILE *fp)
{
	int ret = ferror(fp) ? -1 : 0;
	if (fflush(fp) != 0) ret = -1;
	if (fsync(fileno(fp)) != 0) ret = -1;
	if (fclose(fp) != 0) ret = -1;
	return ret;
}

int timed_snapshot_start(TIMED_SNAPSHOT *state, const char *root,
	uint64_t now_ms)
{
	TIMED_SNAPSHOT fresh = {0};
	char path[384];
	unsigned int attempt;
	int length;
	FILE *fp;

	if (state->active || state->in_flight) {
		printf("Timed Snapshot is already running or finishing a save\n");
		return -1;
	}
	for (attempt = 0; attempt < 10000U; attempt++) {
		length = snprintf(fresh.directory, sizeof(fresh.directory),
			"%s/timed_%lld_%04u", root, (long long)time(NULL), attempt);
		if (length < 0 || (size_t)length >= sizeof(fresh.directory)) return -1;
		if (mkdir(fresh.directory, 0775) == 0) break;
		if (errno != EEXIST) {
			printf("Timed Snapshot: cannot create output directory: %s\n",
				strerror(errno));
			return -1;
		}
	}
	if (attempt == 10000U) return -1;
	snprintf(path, sizeof(path), "%s/summary.csv", fresh.directory);
	fp = fopen(path, "wb");
	if (fp == NULL) {
		printf("Timed Snapshot: cannot create %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(fp, "snapshot,elapsed_ms,sensor_temp_c,sensor_temp_status,"
		"input_file,input_min,input_max,input_mean,input_stddev,"
		"corrected_file,corrected_min,corrected_max,corrected_mean,corrected_stddev,"
		"agc_output_file,agc_output_min,agc_output_max,agc_output_mean,agc_output_stddev\n");
	if (timed_snapshot_close_csv(fp) != 0) {
		printf("Timed Snapshot: cannot flush %s\n", path);
		return -1;
	}
	fresh.active = 1;
	fresh.started_ms = now_ms;
	fresh.next_due_ms = now_ms + TIMED_SNAPSHOT_INTERVAL_MS;
	*state = fresh;
	printf("Timed Snapshot started: every 60 seconds, target=%u; first capture in 60 seconds\n",
		TIMED_SNAPSHOT_COUNT);
	printf("Output: %s\n", state->directory);
	return 0;
}

int timed_snapshot_claim(TIMED_SNAPSHOT *state, uint64_t now_ms)
{
	if (!state->active || state->in_flight || now_ms < state->next_due_ms) return 0;
	state->in_flight = 1;
	state->captured_ms = now_ms;
	state->sensor_temp_c = 0.0f;
	state->sensor_temp_valid = 0;
	return 1;
}

void timed_snapshot_stop(TIMED_SNAPSHOT *state, const char *reason)
{
	if (state->active) {
		state->active = 0;
		state->reason = reason;
		printf("Timed Snapshot stopped: %s; saved=%u/%u%s\n", reason,
			state->saved, TIMED_SNAPSHOT_COUNT,
			state->in_flight ? "; finishing current snapshot" : "");
	}
}

void timed_snapshot_finish(TIMED_SNAPSHOT *state, int result)
{
	if (!state->in_flight) return;
	state->in_flight = 0;
	if (result != 0) {
		state->active = 0;
		state->reason = "snapshot copy or file/summary write failed";
		printf("Timed Snapshot failed at %u/%u; saved=%u. Check SD storage.\n",
			state->saved + 1U, TIMED_SNAPSHOT_COUNT, state->saved);
		return;
	}
	state->saved++;
	/* A delayed frame does not cause a burst of overdue captures. */
	state->next_due_ms = state->captured_ms + TIMED_SNAPSHOT_INTERVAL_MS;
	printf("Timed Snapshot saved %u/%u\n", state->saved, TIMED_SNAPSHOT_COUNT);
	if (state->saved == TIMED_SNAPSHOT_COUNT) {
		state->active = 0;
		state->reason = "completed";
		printf("Timed Snapshot complete: %s/summary.csv\n", state->directory);
	}
}

void timed_snapshot_print_status(const TIMED_SNAPSHOT *state, uint64_t now_ms)
{
	printf("Timed Snapshot: %s, saved=%u/%u\n",
		state->active ? "Running" : (state->in_flight ? "Stopping" : "Stopped"),
		state->saved, TIMED_SNAPSHOT_COUNT);
	if (state->active) {
		uint64_t remaining = state->next_due_ms > now_ms ? state->next_due_ms - now_ms : 0;
		printf("Next capture in %" PRIu64 " s%s\n", (remaining + 999U) / 1000U,
			state->in_flight ? " (saving current snapshot)" : "");
	} else if (state->reason != NULL) {
		printf("Last stop: %s\n", state->reason);
	}
	if (state->directory[0]) printf("Summary: %s/summary.csv\n", state->directory);
}

int timed_snapshot_save(const TIMED_SNAPSHOT *state,
	const unsigned char *input, const unsigned char *corrected,
	const unsigned char *agc, unsigned int width, unsigned int height,
	TIMED_SNAPSHOT_SAVE_RAW save_raw)
{
	const unsigned char *buffers[] = {input, corrected, agc};
	const char *stages[] = {"input", "corrected", "agc_output"};
	RAW_STATS stats[3];
	char names[3][96] = {{0}};
	char path[384];
	size_t pixels = (size_t)width * height;
	unsigned int i;
	int length;
	FILE *fp;

	if (!input || !pixels || !save_raw) return -1;
	for (i = 0; i < 3; i++) {
		if (buffers[i] == NULL) continue;
		length = snprintf(names[i], sizeof(names[i]), "snapshot_%04u_%s_raw16_%ux%u.raw",
			state->saved + 1U, stages[i], width, height);
		if (length < 0 || (size_t)length >= sizeof(names[i])) return -1;
		/* Bound this row explicitly: older GCC may infer the size of the
		 * entire names array when checking a dynamically indexed %s. */
		length = snprintf(path, sizeof(path), "%s/%.*s", state->directory,
			(int)sizeof(names[i]) - 1, names[i]);
		if (length < 0 || (size_t)length >= sizeof(path)) return -1;
		if (save_raw(buffers[i], pixels * 2U, path) != 0) return -1;
		stats[i] = timed_snapshot_stats(buffers[i], pixels);
	}
	snprintf(path, sizeof(path), "%s/summary.csv", state->directory);
	fp = fopen(path, "ab");
	if (fp == NULL) {
		printf("Timed Snapshot: cannot append %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(fp, "%u,%" PRIu64, state->saved + 1U, state->captured_ms - state->started_ms);
	if (state->sensor_temp_valid) {
		fprintf(fp, ",%.4f,ok", (double)state->sensor_temp_c);
	} else {
		fprintf(fp, ",,read_failed");
	}
	for (i = 0; i < 3; i++) {
		if (buffers[i] == NULL) {
			fprintf(fp, ",,,,,");
		} else {
			fprintf(fp, ",%s,%u,%u,%.6f,%.6f", names[i], stats[i].min,
				stats[i].max, stats[i].mean, stats[i].stddev);
		}
	}
	fprintf(fp, "\n");
	return timed_snapshot_close_csv(fp);
}
