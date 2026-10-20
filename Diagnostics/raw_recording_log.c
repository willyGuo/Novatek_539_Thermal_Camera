#include "raw_recording_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int raw_recording_log_close(FILE *fp)
{
	int ret = ferror(fp) ? -1 : 0;
	if (fflush(fp) != 0) ret = -1;
	if (fsync(fileno(fp)) != 0) ret = -1;
	if (fclose(fp) != 0) ret = -1;
	return ret;
}

int raw_recording_log_create(const char *directory)
{
	char path[384];
	FILE *fp;
	int n = snprintf(path, sizeof(path), "%s/summary.csv", directory);
	if (n < 0 || (size_t)n >= sizeof(path)) return -1;
	fp = fopen(path, "wb");
	if (!fp) return -1;
	fprintf(fp, "image,stage,stage_image,elapsed_ms,capture_time_utc,capture_unix_ms,"
		"sensor_temp_c,sensor_temp_status,bpr_enabled,raw_file\n");
	return raw_recording_log_close(fp);
}

int raw_recording_log_append(const char *directory, const RAW_RECORDING_LOG *r)
{
	char path[384], date[32] = "", timestamp[48] = "";
	FILE *fp;
	int n;
	if (!r || !r->stage || !r->raw_file) return -1;
	if (r->capture_unix_ms >= 0) {
		time_t seconds = (time_t)(r->capture_unix_ms / 1000);
		struct tm utc;
#ifdef _WIN32
		int ok = gmtime_s(&utc, &seconds) == 0;
#else
		int ok = gmtime_r(&seconds, &utc) != NULL;
#endif
		if (ok && strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc))
			snprintf(timestamp, sizeof(timestamp), "%s.%03uZ", date,
				(unsigned int)(r->capture_unix_ms % 1000));
	}
	n = snprintf(path, sizeof(path), "%s/summary.csv", directory);
	if (n < 0 || (size_t)n >= sizeof(path)) return -1;
	fp = fopen(path, "ab");
	if (!fp) return -1;
	fprintf(fp, "%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%s,",
		r->sequence, r->stage, r->phase_frame, r->elapsed_ms, timestamp);
	if (r->capture_unix_ms >= 0) fprintf(fp, "%" PRId64, r->capture_unix_ms);
	if (r->sensor_temp_valid) fprintf(fp, ",%.4f,ok", (double)r->sensor_temp_c);
	else fprintf(fp, ",,read_failed");
	fprintf(fp, ",%d,%s\n", r->bpr_enabled, r->raw_file);
	return raw_recording_log_close(fp);
}
