#include "minute_raw_recording.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int minute_raw_close(FILE *fp)
{
	int ret = ferror(fp) ? -1 : 0;
	if (fflush(fp) != 0) ret = -1;
	if (fsync(fileno(fp)) != 0) ret = -1;
	if (fclose(fp) != 0) ret = -1;
	return ret;
}

int minute_raw_start(MINUTE_RAW_RECORDING *s, const char *root,
	uint64_t now_ms, unsigned int minutes)
{
	MINUTE_RAW_RECORDING fresh = {0};
	char path[384];
	unsigned int attempt;
	int n;
	FILE *fp;
	if (s->active || s->in_flight || !root || !minutes || minutes > 1440U) return -1;
	for (attempt = 0; attempt < 10000U; attempt++) {
		n = snprintf(fresh.directory, sizeof(fresh.directory),
			"%s/minute_raw_%lld_%04u", root, (long long)time(NULL), attempt);
		if (n < 0 || (size_t)n >= sizeof(fresh.directory)) return -1;
		if (mkdir(fresh.directory, 0775) == 0) break;
		if (errno != EEXIST) return -1;
	}
	if (attempt == 10000U) return -1;
	snprintf(path, sizeof(path), "%s/summary.csv", fresh.directory);
	fp = fopen(path, "wb");
	if (!fp) return -1;
	fprintf(fp, "pair,minute,frame,elapsed_ms,capture_time_utc,capture_unix_ms,"
		"sensor_temp_c,sensor_temp_status,original_file,corrected_file\n");
	if (minute_raw_close(fp) != 0) return -1;
	fresh.active = 1;
	fresh.started_ms = now_ms;
	fresh.minutes = minutes;
	fresh.captured_unix_ms = -1;
	*s = fresh;
	printf("Minute RAW started: %u minutes, 10 pairs/minute; first batch now.\n", minutes);
	printf("Original + pre-AGC Corrected RAW; capture time is board UTC clock.\n");
	printf("Output: %s\n", s->directory);
	return 0;
}

void minute_raw_stop(MINUTE_RAW_RECORDING *s, const char *reason)
{
	if (!s->active) return;
	s->active = 0;
	s->reason = reason;
	printf("Minute RAW stopped: %s; saved=%" PRIu64 " pairs%s\n", reason,
		s->saved, s->in_flight ? "; finishing current pair" : "");
}

void minute_raw_expire(MINUTE_RAW_RECORDING *s, uint64_t now_ms)
{
	if (s->active && now_ms >= s->started_ms &&
	    now_ms - s->started_ms >= (uint64_t)s->minutes * MINUTE_RAW_INTERVAL_MS)
		minute_raw_stop(s, "duration reached");
}

int minute_raw_claim(MINUTE_RAW_RECORDING *s, uint64_t now_ms)
{
	uint64_t elapsed;
	unsigned int minute;
	minute_raw_expire(s, now_ms);
	if (!s->active || now_ms < s->started_ms) return 0;
	elapsed = now_ms - s->started_ms;
	if (s->in_flight) return 0;
	minute = (unsigned int)(elapsed / MINUTE_RAW_INTERVAL_MS) + 1U;
	if (minute != s->minute) {
		if (s->minute && (s->saved_in_minute < MINUTE_RAW_PAIRS || minute > s->minute + 1U))
			printf("Minute RAW delayed: missed frames/minutes are not backfilled\n");
		s->minute = minute;
		s->saved_in_minute = 0;
	}
	if (s->saved_in_minute == MINUTE_RAW_PAIRS) return 0;
	s->in_flight = 1;
	s->captured_ms = now_ms;
	s->captured_unix_ms = -1;
	s->sensor_temp_c = 0.0f;
	s->sensor_temp_valid = 0;
	return 1;
}

void minute_raw_finish(MINUTE_RAW_RECORDING *s, int result)
{
	if (!s->in_flight) return;
	s->in_flight = 0;
	if (result != 0) {
		s->reason = "RAW copy, storage or CSV write failed";
		s->active = 0;
		printf("Minute RAW failed: saved=%" PRIu64 " pairs; check SD storage\n", s->saved);
		return;
	}
	s->saved++;
	s->saved_in_minute++;
	printf("Minute RAW saved: minute %u/%u, pair %u/10, total=%" PRIu64 "\n",
		s->minute, s->minutes, s->saved_in_minute, s->saved);
}

void minute_raw_status(const MINUTE_RAW_RECORDING *s, uint64_t now_ms)
{
	uint64_t elapsed = now_ms >= s->started_ms ? now_ms - s->started_ms : 0;
	printf("Minute RAW: %s, saved=%" PRIu64 "/%" PRIu64 " pairs, duration=%u min\n",
		s->active ? "Recording" : (s->in_flight ? "Stopping" : "Stopped"),
		s->saved, (uint64_t)s->minutes * MINUTE_RAW_PAIRS, s->minutes);
	if (s->active) printf("Elapsed=%" PRIu64 " s; minute %u, saved %u/10\n",
		elapsed / 1000U, s->minute, s->saved_in_minute);
	if (!s->active && s->reason) printf("Last stop: %s\n", s->reason);
	if (s->directory[0]) printf("Summary: %s/summary.csv\n", s->directory);
}

int minute_raw_save(const MINUTE_RAW_RECORDING *s, const unsigned char *original,
	const unsigned char *corrected, unsigned int width, unsigned int height,
	MINUTE_RAW_SAVE save_raw)
{
	char names[2][96], paths[2][384], csv[384], date[32] = "", timestamp[48] = "";
	const char *stages[] = {"original", "corrected"};
	const unsigned char *buffers[] = {original, corrected};
	size_t bytes = (size_t)width * height * 2U;
	unsigned int i;
	FILE *fp;
	if (!original || !corrected || !bytes || !save_raw) return -1;
	for (i = 0; i < 2U; i++) {
		int n = snprintf(names[i], sizeof(names[i]), "minute_%04u_frame_%02u_%s_raw16_%ux%u.raw",
			s->minute, s->saved_in_minute + 1U, stages[i], width, height);
		if (n < 0 || (size_t)n >= sizeof(names[i])) return -1;
		n = snprintf(paths[i], sizeof(paths[i]), "%s/%.*s", s->directory,
			(int)sizeof(names[i]) - 1, names[i]);
		if (n < 0 || (size_t)n >= sizeof(paths[i])) return -1;
	}
	for (i = 0; i < 2U; i++) {
		if (save_raw(buffers[i], bytes, paths[i]) != 0) {
			remove(paths[0]);
			remove(paths[1]);
			return -1;
		}
	}
	if (s->captured_unix_ms >= 0) {
		time_t seconds = (time_t)(s->captured_unix_ms / 1000);
		struct tm utc;
#ifdef _WIN32
		int time_ok = gmtime_s(&utc, &seconds) == 0;
#else
		int time_ok = gmtime_r(&seconds, &utc) != NULL;
#endif
		if (time_ok && strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc))
			snprintf(timestamp, sizeof(timestamp), "%s.%03uZ", date,
				(unsigned int)(s->captured_unix_ms % 1000));
	}
	snprintf(csv, sizeof(csv), "%s/summary.csv", s->directory);
	fp = fopen(csv, "ab");
	if (!fp) return -1;
	fprintf(fp, "%" PRIu64 ",%u,%u,%" PRIu64 ",%s,", s->saved + 1U,
		s->minute, s->saved_in_minute + 1U, s->captured_ms - s->started_ms, timestamp);
	if (s->captured_unix_ms >= 0) fprintf(fp, "%" PRId64, s->captured_unix_ms);
	if (s->sensor_temp_valid) fprintf(fp, ",%.4f,ok", (double)s->sensor_temp_c);
	else fprintf(fp, ",,read_failed");
	fprintf(fp, ",%s,%s\n", names[0], names[1]);
	return minute_raw_close(fp);
}
