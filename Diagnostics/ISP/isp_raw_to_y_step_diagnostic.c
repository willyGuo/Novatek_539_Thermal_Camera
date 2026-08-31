#include "isp_raw_to_y_step_diagnostic.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ISP_STEP_BLACK_Y_LIMIT    16U
#define ISP_STEP_WHITE_Y_LIMIT    235U
#define ISP_STEP_OUTPUT_ROOT      "/mnt/sd/thermal_diagnostics"
#define ISP_STEP_OUTPUT_DIR       ISP_STEP_OUTPUT_ROOT "/isp_raw_to_y"
#define ISP_STEP_RUN_PREFIX       "run_"
#define ISP_STEP_PATH_SIZE        256U
#define ISP_STEP_FILE_PATH_SIZE   512U
#define ISP_STEP_VERTICAL_MARGIN  8U

typedef struct {
	uint32_t raw_dn;
	uint8_t y_min;
	uint8_t y_median;
	uint8_t y_max;
	double y_mean;
} ISP_STEP_SAMPLE;

typedef struct {
	int valid;
	uint32_t run_id;
	uint32_t min_raw_dn;
	uint32_t max_raw_dn;
	int usable_range_found;
	uint32_t usable_low_dn;
	uint32_t usable_high_dn;
	int last_zero_found;
	uint32_t last_zero_dn;
	int first_nonzero_found;
	uint32_t first_nonzero_dn;
	int first_full_found;
	uint32_t first_full_dn;
	int last_below_full_found;
	uint32_t last_below_full_dn;
	int monotonic;
	double linear_slope;
	double linear_intercept;
	double linear_r_squared;
	char output_dir[ISP_STEP_PATH_SIZE];
} ISP_STEP_LAST_RESULT;

static ISP_STEP_LAST_RESULT g_last_result;

static int isp_step_make_dir(const char *path)
{
	if (mkdir(path, 0775) == 0 || errno == EEXIST) {
		return 0;
	}

	printf("ISP RAW-to-Y Step Test: create %s fail: %s\n",
	       path, strerror(errno));
	return -1;
}

static int isp_step_prepare_output_root(void)
{
	if (isp_step_make_dir(ISP_STEP_OUTPUT_ROOT) != 0) {
		return -1;
	}
	return isp_step_make_dir(ISP_STEP_OUTPUT_DIR);
}

static uint32_t isp_step_raw_dn(uint32_t band, uint32_t min_raw_dn,
				uint32_t max_raw_dn)
{
	return min_raw_dn +
	       (band * (max_raw_dn - min_raw_dn) +
		(ISP_RAW_TO_Y_STEP_COUNT - 1U) / 2U) /
	       (ISP_RAW_TO_Y_STEP_COUNT - 1U);
}

static int isp_step_write_rows(const char *path,
			       const void *buffer,
			       size_t row_bytes,
			       uint32_t stride_bytes,
			       uint32_t height)
{
	const uint8_t *row = (const uint8_t *)buffer;
	FILE *fp;
	uint32_t y;
	int result = 0;

	fp = fopen(path, "wb");
	if (fp == NULL) {
		printf("ISP RAW-to-Y Step Test: open %s fail: %s\n",
		       path, strerror(errno));
		return -1;
	}

	for (y = 0; y < height; y++) {
		if (fwrite(row + (size_t)y * stride_bytes,
			   1, row_bytes, fp) != row_bytes) {
			printf("ISP RAW-to-Y Step Test: write %s fail\n", path);
			result = -1;
			break;
		}
	}
	if (result == 0 && fflush(fp) != 0) {
		printf("ISP RAW-to-Y Step Test: flush %s fail: %s\n",
		       path, strerror(errno));
		result = -1;
	}
	if (result == 0 && fsync(fileno(fp)) != 0) {
		printf("ISP RAW-to-Y Step Test: sync %s fail: %s\n",
		       path, strerror(errno));
		result = -1;
	}
	if (fclose(fp) != 0 && result == 0) {
		printf("ISP RAW-to-Y Step Test: close %s fail: %s\n",
		       path, strerror(errno));
		result = -1;
	}

	return result;
}

static int isp_step_finish_text_file(FILE *fp, const char *path)
{
	int result = 0;

	if (fflush(fp) != 0) {
		result = -1;
	}
	if (result == 0 && fsync(fileno(fp)) != 0) {
		result = -1;
	}
	if (fclose(fp) != 0) {
		result = -1;
	}
	if (result != 0) {
		printf("ISP RAW-to-Y Step Test: save %s fail: %s\n",
		       path, strerror(errno));
	}

	return result;
}

static uint8_t isp_step_histogram_median(const uint32_t histogram[256],
					 uint64_t sample_count)
{
	uint64_t target = (sample_count + 1U) / 2U;
	uint64_t cumulative = 0;
	uint32_t value;

	for (value = 0; value < 256U; value++) {
		cumulative += histogram[value];
		if (cumulative >= target) {
			return (uint8_t)value;
		}
	}

	return 255U;
}

static int isp_step_analyze_bands(const uint8_t *y_plane,
				  uint32_t y_stride_bytes,
				  uint32_t width,
				  uint32_t height,
				  uint32_t min_raw_dn,
				  uint32_t max_raw_dn,
				  ISP_STEP_SAMPLE samples[ISP_RAW_TO_Y_STEP_COUNT],
				  uint32_t *usable_low_dn,
				  uint32_t *usable_high_dn,
				  int *last_zero_found,
				  uint32_t *last_zero_dn,
				  int *first_nonzero_found,
				  uint32_t *first_nonzero_dn,
				  int *first_full_found,
				  uint32_t *first_full_dn,
				  int *last_below_full_found,
				  uint32_t *last_below_full_dn,
				  int *monotonic,
				  double *linear_slope,
				  double *linear_intercept,
				  double *linear_r_squared)
{
	uint32_t band_width;
	uint32_t band;
	int low_index = -1;
	int high_index = -1;
	double sum_x = 0.0;
	double sum_y = 0.0;
	double sum_xx = 0.0;
	double sum_xy = 0.0;
	double mean_y;
	double slope;
	double intercept;
	double ss_total = 0.0;
	double ss_residual = 0.0;
	uint32_t fit_count = 0;

	if (y_plane == NULL || samples == NULL || usable_low_dn == NULL ||
	    usable_high_dn == NULL || last_zero_found == NULL ||
	    last_zero_dn == NULL || first_nonzero_found == NULL ||
	    first_nonzero_dn == NULL || first_full_found == NULL ||
	    first_full_dn == NULL || last_below_full_found == NULL ||
	    last_below_full_dn == NULL || monotonic == NULL ||
	    linear_slope == NULL || linear_intercept == NULL ||
	    linear_r_squared == NULL || width < ISP_RAW_TO_Y_STEP_COUNT ||
	    height <= ISP_STEP_VERTICAL_MARGIN * 2U || y_stride_bytes < width) {
		return -1;
	}

	band_width = width / ISP_RAW_TO_Y_STEP_COUNT;
	*monotonic = 1;
	*last_zero_found = 0;
	*first_nonzero_found = 0;
	*first_full_found = 0;
	*last_below_full_found = 0;
	for (band = 0; band < ISP_RAW_TO_Y_STEP_COUNT; band++) {
		uint32_t x_begin = band * band_width;
		uint32_t x_end = band == ISP_RAW_TO_Y_STEP_COUNT - 1U ?
			width : (band + 1U) * band_width;
		uint32_t margin = band_width >= 5U ? 2U : 0U;
		uint32_t histogram[256] = {0};
		uint64_t sum = 0;
		uint64_t count = 0;
		uint32_t x;
		uint32_t y;
		uint32_t value;

		x_begin += margin;
		x_end -= margin;
		for (y = ISP_STEP_VERTICAL_MARGIN;
		     y < height - ISP_STEP_VERTICAL_MARGIN; y++) {
			const uint8_t *row = y_plane + (size_t)y * y_stride_bytes;
			for (x = x_begin; x < x_end; x++) {
				histogram[row[x]]++;
				sum += row[x];
				count++;
			}
		}

		samples[band].raw_dn = isp_step_raw_dn(
			band, min_raw_dn, max_raw_dn);
		samples[band].y_min = 255U;
		samples[band].y_max = 0U;
		for (value = 0; value < 256U; value++) {
			if (histogram[value] != 0U) {
				if (samples[band].y_min == 255U) {
					samples[band].y_min = (uint8_t)value;
				}
				samples[band].y_max = (uint8_t)value;
			}
		}
		samples[band].y_median =
			isp_step_histogram_median(histogram, count);
		samples[band].y_mean = (double)sum / (double)count;

		if (band > 0U &&
		    samples[band].y_median < samples[band - 1U].y_median) {
			*monotonic = 0;
		}
		if (low_index < 0 &&
		    samples[band].y_median > ISP_STEP_BLACK_Y_LIMIT) {
			low_index = (int)band;
		}
		if (samples[band].y_median < ISP_STEP_WHITE_Y_LIMIT) {
			high_index = (int)band;
		}
		if (samples[band].y_median == 0U) {
			*last_zero_found = 1;
			*last_zero_dn = samples[band].raw_dn;
		} else if (!*first_nonzero_found) {
			*first_nonzero_found = 1;
			*first_nonzero_dn = samples[band].raw_dn;
		}
		if (samples[band].y_median < 255U) {
			*last_below_full_found = 1;
			*last_below_full_dn = samples[band].raw_dn;
		} else if (!*first_full_found) {
			*first_full_found = 1;
			*first_full_dn = samples[band].raw_dn;
		}
	}

	if (low_index < 0 || high_index < low_index) {
		*usable_low_dn = 0U;
		*usable_high_dn = 0U;
		*linear_slope = 0.0;
		*linear_intercept = 0.0;
		*linear_r_squared = 0.0;
		return 1;
	}

	*usable_low_dn = samples[low_index].raw_dn;
	*usable_high_dn = samples[high_index].raw_dn;
	for (band = (uint32_t)low_index; band <= (uint32_t)high_index; band++) {
		double x = (double)samples[band].raw_dn;
		double y = samples[band].y_mean;
		sum_x += x;
		sum_y += y;
		sum_xx += x * x;
		sum_xy += x * y;
		fit_count++;
	}

	if (fit_count < 2U ||
	    (double)fit_count * sum_xx - sum_x * sum_x == 0.0) {
		*linear_slope = 0.0;
		*linear_intercept = 0.0;
		*linear_r_squared = 0.0;
		return 0;
	}

	slope = ((double)fit_count * sum_xy - sum_x * sum_y) /
		((double)fit_count * sum_xx - sum_x * sum_x);
	intercept = (sum_y - slope * sum_x) / (double)fit_count;
	*linear_slope = slope;
	*linear_intercept = intercept;
	mean_y = sum_y / (double)fit_count;
	for (band = (uint32_t)low_index; band <= (uint32_t)high_index; band++) {
		double actual = samples[band].y_mean;
		double predicted = slope * (double)samples[band].raw_dn + intercept;
		double total_delta = actual - mean_y;
		double residual = actual - predicted;
		ss_total += total_delta * total_delta;
		ss_residual += residual * residual;
	}
	*linear_r_squared = ss_total > 0.0 ?
		1.0 - ss_residual / ss_total : 1.0;

	return 0;
}

int isp_raw_to_y_step_diagnostic_get_next_run_id(uint32_t *run_id)
{
	DIR *dir;
	struct dirent *entry;
	uint32_t max_id = 0;
	int found = 0;
	const size_t prefix_length = sizeof(ISP_STEP_RUN_PREFIX) - 1U;

	if (run_id == NULL || isp_step_prepare_output_root() != 0) {
		return -1;
	}

	dir = opendir(ISP_STEP_OUTPUT_DIR);
	if (dir == NULL) {
		printf("ISP RAW-to-Y Step Test: scan %s fail: %s\n",
		       ISP_STEP_OUTPUT_DIR, strerror(errno));
		return -1;
	}
	while ((entry = readdir(dir)) != NULL) {
		char *end;
		unsigned long value;

		if (strncmp(entry->d_name, ISP_STEP_RUN_PREFIX,
			    prefix_length) != 0) {
			continue;
		}
		errno = 0;
		value = strtoul(entry->d_name + prefix_length, &end, 10);
		if (errno == ERANGE || end == entry->d_name + prefix_length ||
		    *end != '\0' || value > UINT_MAX) {
			continue;
		}
		if (!found || value > max_id) {
			max_id = (uint32_t)value;
			found = 1;
		}
	}
	closedir(dir);

	if (found && max_id == UINT_MAX) {
		printf("ISP RAW-to-Y Step Test: run sequence is exhausted\n");
		return -1;
	}
	*run_id = found ? max_id + 1U : 0U;
	return 0;
}

int isp_raw_to_y_step_diagnostic_generate(uint16_t *raw,
				    uint32_t raw_stride_bytes,
				    uint32_t width,
				    uint32_t height,
				    uint32_t min_raw_dn,
				    uint32_t max_raw_dn)
{
	uint32_t band_width;
	uint32_t x;
	uint32_t y;

	if (raw == NULL || width < ISP_RAW_TO_Y_STEP_COUNT || height == 0U ||
	    min_raw_dn >= max_raw_dn ||
	    max_raw_dn > ISP_RAW_TO_Y_STEP_RAW14_MAX ||
	    raw_stride_bytes < width * sizeof(uint16_t) ||
	    (raw_stride_bytes & 1U) != 0U) {
		return -1;
	}

	band_width = width / ISP_RAW_TO_Y_STEP_COUNT;
	for (y = 0; y < height; y++) {
		uint16_t *row = (uint16_t *)((uint8_t *)raw +
					    (size_t)y * raw_stride_bytes);
		for (x = 0; x < width; x++) {
			uint32_t band = x / band_width;
			if (band >= ISP_RAW_TO_Y_STEP_COUNT) {
				band = ISP_RAW_TO_Y_STEP_COUNT - 1U;
			}
			row[x] = (uint16_t)isp_step_raw_dn(
				band, min_raw_dn, max_raw_dn);
		}
	}

	return 0;
}

int isp_raw_to_y_step_diagnostic_save_and_analyze(
	uint32_t run_id,
	uint32_t min_raw_dn,
	uint32_t max_raw_dn,
	const uint16_t *raw,
	uint32_t raw_stride_bytes,
	const uint8_t *y_plane,
	uint32_t y_stride_bytes,
	uint32_t width,
	uint32_t height)
{
	ISP_STEP_SAMPLE samples[ISP_RAW_TO_Y_STEP_COUNT];
	uint32_t usable_low_dn;
	uint32_t usable_high_dn;
	int last_zero_found;
	uint32_t last_zero_dn = 0U;
	int first_nonzero_found;
	uint32_t first_nonzero_dn = 0U;
	int first_full_found;
	uint32_t first_full_dn = 0U;
	int last_below_full_found;
	uint32_t last_below_full_dn = 0U;
	int monotonic;
	double linear_slope;
	double linear_intercept;
	double linear_r_squared;
	int analysis_status;
	char run_dir[ISP_STEP_PATH_SIZE];
	char path[ISP_STEP_FILE_PATH_SIZE];
	FILE *fp;
	uint32_t band;

	if (raw == NULL || y_plane == NULL || min_raw_dn >= max_raw_dn ||
	    max_raw_dn > ISP_RAW_TO_Y_STEP_RAW14_MAX ||
	    isp_step_prepare_output_root() != 0) {
		return -1;
	}

	snprintf(run_dir, sizeof(run_dir), "%s/run_%04u",
		 ISP_STEP_OUTPUT_DIR, run_id);
	if (isp_step_make_dir(run_dir) != 0) {
		return -1;
	}

	snprintf(path, sizeof(path), "%s/input_step_raw16_%ux%u.raw",
		 run_dir, width, height);
	if (isp_step_write_rows(path, raw, (size_t)width * sizeof(uint16_t),
				 raw_stride_bytes, height) != 0) {
		return -1;
	}

	snprintf(path, sizeof(path), "%s/output_videoproc_y8_%ux%u.raw",
		 run_dir, width, height);
	if (isp_step_write_rows(path, y_plane, width,
				 y_stride_bytes, height) != 0) {
		return -1;
	}

	analysis_status = isp_step_analyze_bands(
		y_plane, y_stride_bytes, width, height,
		min_raw_dn, max_raw_dn,
		samples, &usable_low_dn, &usable_high_dn,
		&last_zero_found, &last_zero_dn,
		&first_nonzero_found, &first_nonzero_dn,
		&first_full_found, &first_full_dn,
		&last_below_full_found, &last_below_full_dn,
		&monotonic, &linear_slope, &linear_intercept,
		&linear_r_squared);
	if (analysis_status < 0) {
		printf("ISP RAW-to-Y Step Test: analyze output failed\n");
		return -1;
	}

	snprintf(path, sizeof(path), "%s/raw_to_y.csv", run_dir);
	fp = fopen(path, "w");
	if (fp == NULL) {
		printf("ISP RAW-to-Y Step Test: open %s fail: %s\n",
		       path, strerror(errno));
		return -1;
	}
	fprintf(fp, "band,raw_dn,y_min,y_mean,y_median,y_max\n");
	for (band = 0; band < ISP_RAW_TO_Y_STEP_COUNT; band++) {
		fprintf(fp, "%u,%u,%u,%.4f,%u,%u\n",
			band, samples[band].raw_dn, samples[band].y_min,
			samples[band].y_mean, samples[band].y_median,
			samples[band].y_max);
	}
	if (isp_step_finish_text_file(fp, path) != 0) {
		return -1;
	}

	snprintf(path, sizeof(path), "%s/summary.txt", run_dir);
	fp = fopen(path, "w");
	if (fp == NULL) {
		printf("ISP RAW-to-Y Step Test: open %s fail: %s\n",
		       path, strerror(errno));
		return -1;
	}
	fprintf(fp, "ISP RAW-container-to-Y8 Step Test\n");
	fprintf(fp, "resolution=%ux%u\n", width, height);
	fprintf(fp, "step_count=%u\n", ISP_RAW_TO_Y_STEP_COUNT);
	fprintf(fp, "input_min_raw_dn=%u\n", min_raw_dn);
	fprintf(fp, "input_max_raw_dn=%u\n", max_raw_dn);
	fprintf(fp, "black_limit_y=%u\n", ISP_STEP_BLACK_Y_LIMIT);
	fprintf(fp, "white_limit_y=%u\n", ISP_STEP_WHITE_Y_LIMIT);
	fprintf(fp, "raw_format=uint16_little_endian_lower14_bit\n");
	fprintf(fp, "usable_range_found=%s\n",
		analysis_status == 0 ? "yes" : "no");
	if (analysis_status == 0) {
		fprintf(fp, "estimated_usable_low_raw_dn=%u\n", usable_low_dn);
		fprintf(fp, "estimated_usable_high_raw_dn=%u\n", usable_high_dn);
	}
	fprintf(fp, "last_y0_raw_dn=%s", last_zero_found ? "" : "not_found\n");
	if (last_zero_found) {
		fprintf(fp, "%u\n", last_zero_dn);
	}
	fprintf(fp, "first_y_nonzero_raw_dn=%s",
		first_nonzero_found ? "" : "not_found\n");
	if (first_nonzero_found) {
		fprintf(fp, "%u\n", first_nonzero_dn);
	}
	fprintf(fp, "last_y_below_255_raw_dn=%s",
		last_below_full_found ? "" : "not_found\n");
	if (last_below_full_found) {
		fprintf(fp, "%u\n", last_below_full_dn);
	}
	fprintf(fp, "first_y255_raw_dn=%s",
		first_full_found ? "" : "not_found\n");
	if (first_full_found) {
		fprintf(fp, "%u\n", first_full_dn);
	}
	fprintf(fp, "median_monotonic=%s\n", monotonic ? "yes" : "no");
	fprintf(fp, "linear_fit_y_equals=%.9f * RAW_DN + %.6f\n",
		linear_slope, linear_intercept);
	fprintf(fp, "linear_fit_r_squared=%.6f\n", linear_r_squared);
	if (isp_step_finish_text_file(fp, path) != 0) {
		return -1;
	}

	memset(&g_last_result, 0, sizeof(g_last_result));
	g_last_result.valid = 1;
	g_last_result.run_id = run_id;
	g_last_result.min_raw_dn = min_raw_dn;
	g_last_result.max_raw_dn = max_raw_dn;
	g_last_result.usable_range_found = analysis_status == 0;
	g_last_result.usable_low_dn = usable_low_dn;
	g_last_result.usable_high_dn = usable_high_dn;
	g_last_result.last_zero_found = last_zero_found;
	g_last_result.last_zero_dn = last_zero_dn;
	g_last_result.first_nonzero_found = first_nonzero_found;
	g_last_result.first_nonzero_dn = first_nonzero_dn;
	g_last_result.first_full_found = first_full_found;
	g_last_result.first_full_dn = first_full_dn;
	g_last_result.last_below_full_found = last_below_full_found;
	g_last_result.last_below_full_dn = last_below_full_dn;
	g_last_result.monotonic = monotonic;
	g_last_result.linear_slope = linear_slope;
	g_last_result.linear_intercept = linear_intercept;
	g_last_result.linear_r_squared = linear_r_squared;
	snprintf(g_last_result.output_dir, sizeof(g_last_result.output_dir),
		 "%s", run_dir);

	printf("ISP RAW-to-Y Step Test %04u complete\n", run_id);
	printf("input RAW DN step range: %u..%u\n", min_raw_dn, max_raw_dn);
	if (analysis_status == 0) {
		printf("estimated usable input range by Y=17..234: %u..%u\n",
		       usable_low_dn, usable_high_dn);
	} else {
		printf("no usable input range reached Y=17..234; "
		       "CSV and RAW outputs were still saved\n");
	}
	printf("black transition: last Y=0 RAW=%s",
	       last_zero_found ? "" : "not found\n");
	if (last_zero_found) {
		printf("%u, first Y>0 RAW=%s", last_zero_dn,
		       first_nonzero_found ? "" : "not found\n");
		if (first_nonzero_found) {
			printf("%u\n", first_nonzero_dn);
		}
	}
	printf("white transition: last Y<255 RAW=%s",
	       last_below_full_found ? "" : "not found\n");
	if (last_below_full_found) {
		printf("%u, first Y=255 RAW=%s", last_below_full_dn,
		       first_full_found ? "" : "not found\n");
		if (first_full_found) {
			printf("%u\n", first_full_dn);
		}
	}
	printf("linear fit: Y=%.9f*RAW_DN%+.6f\n",
	       linear_slope, linear_intercept);
	printf("median monotonic=%s, linear fit R^2=%.6f\n",
	       monotonic ? "yes" : "no", linear_r_squared);
	printf("result directory: %s\n", run_dir);

	return 0;
}

void isp_raw_to_y_step_diagnostic_print_last_result(void)
{
	if (!g_last_result.valid) {
		printf("ISP RAW-to-Y Step Test: no completed result in this run\n");
		return;
	}

	printf("ISP RAW-to-Y Step Test %04u last result\n",
	       g_last_result.run_id);
	printf("input RAW DN step range: %u..%u\n",
	       g_last_result.min_raw_dn, g_last_result.max_raw_dn);
	if (g_last_result.usable_range_found) {
		printf("estimated usable input range by Y=17..234: %u..%u\n",
		       g_last_result.usable_low_dn,
		       g_last_result.usable_high_dn);
	} else {
		printf("no usable input range reached Y=17..234\n");
	}
	printf("linear fit: Y=%.9f*RAW_DN%+.6f\n",
	       g_last_result.linear_slope, g_last_result.linear_intercept);
	printf("median monotonic=%s, linear fit R^2=%.6f\n",
	       g_last_result.monotonic ? "yes" : "no",
	       g_last_result.linear_r_squared);
	printf("result directory: %s\n", g_last_result.output_dir);
}
