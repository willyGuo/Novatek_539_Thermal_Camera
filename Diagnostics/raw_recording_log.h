#ifndef RAW_RECORDING_LOG_H
#define RAW_RECORDING_LOG_H

#include <stdint.h>

typedef struct {
	uint64_t sequence, phase_frame, elapsed_ms;
	int64_t capture_unix_ms;
	float sensor_temp_c;
	int sensor_temp_valid, bpr_enabled;
	const char *stage;
	const char *raw_file;
} RAW_RECORDING_LOG;

int raw_recording_log_create(const char *directory);
int raw_recording_log_append(const char *directory, const RAW_RECORDING_LOG *record);

#endif
