#ifndef MINUTE_RAW_RECORDING_H
#define MINUTE_RAW_RECORDING_H

#include <stddef.h>
#include <stdint.h>

#define MINUTE_RAW_PAIRS 10U
#define MINUTE_RAW_INTERVAL_MS 60000U

/* Protected by the snapshot writer lock. While in_flight, capture metadata
 * and paths are immutable after publication until the background save ends.
 * Stop changes only active/reason; restart must wait for the writer to drain. */
typedef struct {
	int active, in_flight;
	unsigned int minutes, minute, saved_in_minute;
	uint64_t saved, started_ms, captured_ms;
	int64_t captured_unix_ms;
	float sensor_temp_c;
	int sensor_temp_valid;
	char directory[256];
	const char *reason;
} MINUTE_RAW_RECORDING;

typedef int (*MINUTE_RAW_SAVE)(const unsigned char *, size_t, const char *);
int minute_raw_start(MINUTE_RAW_RECORDING *s, const char *root,
	uint64_t now_ms, unsigned int minutes);
int minute_raw_claim(MINUTE_RAW_RECORDING *s, uint64_t now_ms);
void minute_raw_expire(MINUTE_RAW_RECORDING *s, uint64_t now_ms);
void minute_raw_stop(MINUTE_RAW_RECORDING *s, const char *reason);
void minute_raw_finish(MINUTE_RAW_RECORDING *s, int result);
void minute_raw_status(const MINUTE_RAW_RECORDING *s, uint64_t now_ms);
int minute_raw_save(const MINUTE_RAW_RECORDING *s, const unsigned char *original,
	const unsigned char *corrected, unsigned int width, unsigned int height,
	MINUTE_RAW_SAVE save_raw);

#endif
