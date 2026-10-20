#ifndef TIMED_SNAPSHOT_H
#define TIMED_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

#define TIMED_SNAPSHOT_COUNT 32U
#define TIMED_SNAPSHOT_INTERVAL_MS 60000U

/* State is serialized by liveview's snapshot writer lock. While in_flight,
 * the writer may read directory/saved/captured_ms and temperature metadata
 * without holding that lock after the capture thread publishes the job;
 * stop only changes active/reason, and start must reject an in-flight job. */
typedef struct {
	int active;
	int in_flight;
	unsigned int saved;
	uint64_t started_ms;
	uint64_t next_due_ms;
	uint64_t captured_ms;
	float sensor_temp_c;
	int sensor_temp_valid;
	char directory[256];
	const char *reason;
} TIMED_SNAPSHOT;

typedef int (*TIMED_SNAPSHOT_SAVE_RAW)(const unsigned char *, size_t,
	const char *);

int timed_snapshot_start(TIMED_SNAPSHOT *state, const char *root,
	uint64_t now_ms);
int timed_snapshot_claim(TIMED_SNAPSHOT *state, uint64_t now_ms);
void timed_snapshot_stop(TIMED_SNAPSHOT *state, const char *reason);
void timed_snapshot_finish(TIMED_SNAPSHOT *state, int result);
void timed_snapshot_print_status(const TIMED_SNAPSHOT *state, uint64_t now_ms);
/* Runs on the background writer with stable, privately copied RAW buffers.
 * A NULL corrected/AGC buffer produces empty CSV cells for that stage. */
int timed_snapshot_save(const TIMED_SNAPSHOT *state,
	const unsigned char *input, const unsigned char *corrected,
	const unsigned char *agc, unsigned int width, unsigned int height,
	TIMED_SNAPSHOT_SAVE_RAW save_raw);

#endif
