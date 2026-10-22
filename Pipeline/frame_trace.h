#ifndef FRAME_TRACE_H
#define FRAME_TRACE_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "frame_diagnostics.h"

/* Diagnostic only: timestamps are observed, never rewritten or filtered.
 * Relative lag growth compares elapsed PTS with elapsed monotonic time in this
 * stage; it is not absolute capture-to-display latency. Zero PTS is unavailable.
 */
typedef struct {
    uint64_t seen, last_count, last_pts, last_wall;
    uint64_t origin_pts, origin_wall, last_alert_wall;
    uint64_t pts_back, pts_repeat, count_back, gaps, lag_events, zero_pts;
    uint64_t max_gap_us, max_lag_growth_us;
    int initialized, lag_high;
    unsigned int sample_phase;
} FRAME_TRACE;

static inline uint64_t frame_trace_now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000U + (uint64_t)ts.tv_nsec / 1000U;
}

/* Explicit time argument also permits deterministic host fault tests. */
static inline void frame_trace_sample_at(FRAME_TRACE *trace, const char *stage,
    uint64_t count, uint64_t pts, uint64_t wall)
{
#if !FRAME_DIAGNOSTICS_ENABLED
    (void)trace; (void)stage; (void)count; (void)pts; (void)wall;
#else
    uint64_t previous_pts = trace->last_pts, previous_count = trace->last_count;
    uint64_t gap = 0, lag_growth = 0;
    int alert = 0, reset_origin = 0;
    if (!trace->seen) {
        const unsigned char *p = (const unsigned char *)stage;
        unsigned int hash = 0;
        while (*p) hash = hash * 33U + *p++;
        trace->sample_phase = hash % 299U + 1U;
    }
    trace->seen++;
    if (!pts) trace->zero_pts++;
    if (trace->initialized) {
        if (count && previous_count && count < previous_count) {
            trace->count_back++; alert = 1;
        }
        if (pts && previous_pts) {
            if (pts < previous_pts) {
                trace->pts_back++; alert = reset_origin = 1;
            } else if (pts == previous_pts) {
                trace->pts_repeat++; alert = 1;
            }
        }
        if (wall >= trace->last_wall) gap = wall - trace->last_wall;
        else reset_origin = 1;
        if (gap > trace->max_gap_us) trace->max_gap_us = gap;
        if (gap > 200000U) { trace->gaps++; alert = 1; }
    }
    if (!trace->initialized || reset_origin || !pts || !trace->origin_pts) {
        trace->origin_pts = pts;
        trace->origin_wall = wall;
        trace->lag_high = 0;
    } else if (pts >= trace->origin_pts && wall >= trace->origin_wall) {
        uint64_t wall_elapsed = wall - trace->origin_wall;
        uint64_t pts_elapsed = pts - trace->origin_pts;
        if (wall_elapsed > pts_elapsed) lag_growth = wall_elapsed - pts_elapsed;
        if (lag_growth > trace->max_lag_growth_us)
            trace->max_lag_growth_us = lag_growth;
        if (lag_growth > 200000U && !trace->lag_high) {
            trace->lag_events++; alert = 1;
        }
        trace->lag_high = lag_growth > 200000U;
    }
    /* Limit anomaly output to one line/second; totals still count every event. */
    if (trace->seen == 1 || trace->seen % 300U == trace->sample_phase ||
        (alert && (!trace->last_alert_wall ||
            (wall >= trace->last_alert_wall && wall - trace->last_alert_wall >= 1000000U)))) {
        char detail[192] = {0};
        if (alert)
            snprintf(detail, sizeof(detail),
                " prev_count=%llu prev_pts=%llu gap_us=%llu lag_growth_us=%llu",
                (unsigned long long)previous_count, (unsigned long long)previous_pts,
                (unsigned long long)gap, (unsigned long long)lag_growth);
        printf("FRAME_TRACE %s stage=%s n=%llu count=%llu pts=%llu wall=%llu "
            "back=%llu dup=%llu cback=%llu zero=%llu maxgap_us=%llu maxlag_us=%llu%s\n",
            alert ? "event" : "sample", stage,
            (unsigned long long)trace->seen, (unsigned long long)count,
            (unsigned long long)pts, (unsigned long long)wall,
            (unsigned long long)trace->pts_back, (unsigned long long)trace->pts_repeat,
            (unsigned long long)trace->count_back, (unsigned long long)trace->zero_pts,
            (unsigned long long)trace->max_gap_us,
            (unsigned long long)trace->max_lag_growth_us, detail);
        if (alert) trace->last_alert_wall = wall;
    }
    trace->initialized = 1;
    trace->last_count = count;
    trace->last_pts = pts;
    trace->last_wall = wall;
#endif
}

static inline void frame_trace_sample(FRAME_TRACE *trace, const char *stage,
    uint64_t count, uint64_t pts)
{
#if FRAME_DIAGNOSTICS_ENABLED
    frame_trace_sample_at(trace, stage, count, pts, frame_trace_now_us());
#else
    (void)trace; (void)stage; (void)count; (void)pts;
#endif
}
#endif
