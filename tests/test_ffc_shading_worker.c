/* Exercise the actual extracted RAW-worker calibration functions with mocks. */
#define main ffc_table_tests_main
#include "test_ffc_shading.c"
#undef main
#include "../Enhancement/AGC/agc_weighted.h"

#define RAW_SIZE_W THERMAL_SENSOR_WIDTH
#define RAW_SIZE_H THERMAL_SENSOR_HEIGHT
#define RAW_ACTIVE_W RAW_SIZE_W
#define RAW_ACTIVE_H RAW_SIZE_H
#define RAW_ACTIVE_X 0
#define RAW_ACTIVE_Y 0
#define CAP_OUT_FMT 0
#define VDO_RAW_BUFSIZE(w,h,fmt) ((w)*(h)*2U)
#define FFC_AVERAGE_FRAME_COUNT 4U
#define SHUTTER_CALIBRATION_PAUSED 1
typedef struct {
    int cap_exit;
    UINTPTR thermal_ffc_out_va, thermal_ffc_out_pa;
    UINTPTR thermal_tdc_out_pa, thermal_nuc_out_pa;
    UINTPTR thermal_k_frame_pa, thermal_b_frame_pa;
    HD_COMMON_MEM_VB_BLK thermal_tdc_out_blk, thermal_nuc_out_blk;
    AGC_WEIGHTED_WORKSPACE *agc_weighted_workspace;
} VIDEO_LIVEVIEW;
static pthread_mutex_t g_shutter_calibration_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_manual_calibration_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_manual_calibration_active, g_shutter_close_wait_ms = 200;
static int shutter_closed, pull_count, release_count, held, open_count;
static int fail_pull, fail_open, fail_close, fail_map, enabled_nuc = 1;
static int fail_release;
static int dynamic_nuc, update_count, reset_count, post_open_reset_count;
static UINT32 nuc_epoch = 1, tdc_epoch = 1, g_weighted_calibration_generation;
static int queued_frames, queued_pulls, queue_on_transition;
static UINT16 *sensor_raw;
static UINT32 scene_level = 12000;
static int ordinary_average_test;
static const UINT16 ordinary_frames[4][4] = {
    {100, 16383, 0, 9198}, {200, 16383, 0, 9202},
    {300, 16383, 0, 9199}, {400, 16383, 0, 9205}
};
static const UINT16 ordinary_expected[4] = {250, 16383, 0, 9201};
static int wide_average_test;
static int expect_correction_lock;

static int nuc_is_enabled(void) { return enabled_nuc; }
static int nuc_is_blackbody_dynamic_enabled(void) { return dynamic_nuc; }
static int tdc_is_enabled(void) { return 0; }
static UINT32 nuc_correction_generation(void) { return nuc_epoch; }
static UINT32 tdc_correction_generation(void) { return tdc_epoch; }
static int nuc_prepare_blackbody_dynamic_table(HD_VIDEO_FRAME *f) { (void)f; return 0; }
static void liveview_sleep_ms(int ms) { (void)ms; }
static void liveview_update_temperature_tables_if_needed(UINT64 *n, UINT64 *t)
{ (void)n; (void)t; ++update_count; }
static int gpio_shutter_open(void)
{ ++open_count; if (open_count == fail_open) return -1; shutter_closed = 0;
  if (queue_on_transition) queued_frames = 2;
  return 0; }
static int gpio_shutter_close(void)
{ shutter_closed = 1; if (queue_on_transition) queued_frames = 2; return fail_close ? -1 : 0; }
static HD_RESULT liveview_cap_pull(VIDEO_LIVEVIEW *s, HD_VIDEO_FRAME *f, int timeout)
{
    size_t i;
    (void)s;
    if (expect_correction_lock)
        assert(pthread_mutex_trylock(&g_manual_calibration_lock) != 0);
    if (!timeout) {
        if (!queued_frames) return HD_ERR_UNDERRUN;
        --queued_frames; ++queued_pulls;
        assert(!held); held = 1;
        for (i = 0; i < FFC_SHADING_PIXELS; ++i) sensor_raw[i] = 1234;
        f->phy_addr[0] = (UINTPTR)sensor_raw; f->loff[0] = RAW_SIZE_W * 2;
        return HD_OK;
    }
    ++pull_count;
    if (pull_count == fail_pull) return HD_ERR_NG;
    assert(!held); held = 1;
    for (i = 0; i < FFC_SHADING_PIXELS; ++i)
        sensor_raw[i] = (UINT16)((shutter_closed ? (i < FFC_SHADING_PIXELS / 2 ? 10020 : 10000) : scene_level)
            + pull_count % 2);
    if (ordinary_average_test)
        for (i = 0; i < FFC_SHADING_PIXELS; ++i)
            sensor_raw[i] = ordinary_frames[(pull_count - 1) % 4][i % 4];
    if (wide_average_test)
        for (i = 0; i < FFC_SHADING_PIXELS; ++i)
            sensor_raw[i] = i % 2 ? 65535 : 20000;
    f->phy_addr[0] = (UINTPTR)sensor_raw; f->loff[0] = RAW_SIZE_W * 2;
    return HD_OK;
}
static HD_RESULT liveview_cap_release(VIDEO_LIVEVIEW *s, HD_VIDEO_FRAME *f)
{ (void)s; (void)f; assert(held); held = 0; ++release_count;
  return release_count == fail_release ? HD_ERR_NG : HD_OK; }
void *hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE type, UINTPTR pa, UINT32 size)
{ (void)type; (void)size; return fail_map ? NULL : (void *)pa; }
HD_RESULT hd_common_mem_munmap(void *va, unsigned int size)
{ (void)va; (void)size; return HD_OK; }
static HD_RESULT nuc_apply(HD_VIDEO_FRAME *in, HD_VIDEO_FRAME *out,
    UINTPTR k, UINTPTR b, UINTPTR dst, HD_COMMON_MEM_VB_BLK blk, UINT32 w, UINT32 h)
{
    size_t i;
    (void)k; (void)b; (void)blk;
    *out = *in; out->phy_addr[0] = dst;
    for (i = 0; i < (size_t)w*h; ++i) ((UINT16 *)dst)[i] = ((UINT16 *)in->phy_addr[0])[i] + 500U;
    return HD_OK;
}
static HD_RESULT tdc_apply(HD_VIDEO_FRAME *in, HD_VIDEO_FRAME *out,
    UINTPTR dst, HD_COMMON_MEM_VB_BLK blk, UINT32 w, UINT32 h)
{ return nuc_apply(in, out, 0, 0, dst, blk, w, h); }
void agc_weighted_state_reset(AGC_WEIGHTED_WORKSPACE *ws)
{
    assert(ws); ++reset_count;
    if (!shutter_closed && pull_count >= 12 && !held && !queued_frames)
        ++post_open_reset_count;
}
static int liveview_update_ffc_table_from_average(VIDEO_LIVEVIEW *s, UINT32 stride)
{
    size_t i;
    if (wide_average_test) {
        for (i = 0; i < FFC_SHADING_PIXELS; ++i)
            assert(((UINT16 *)s->thermal_ffc_out_va)[i] == (i % 2 ? 65535 : 20000));
    }
    if (ordinary_average_test)
    for (i = 0; i < FFC_SHADING_PIXELS; ++i)
        assert(((UINT16 *)s->thermal_ffc_out_va)[i] == ordinary_expected[i % 4]);
    return ffc_update_table_from_raw(s->thermal_ffc_out_va, stride,
        0, 0, RAW_ACTIVE_W, RAW_ACTIVE_H);
}
static int liveview_run_ffc_gfx_test(VIDEO_LIVEVIEW *s, UINT32 stride)
{ (void)s; (void)stride; return 0; }

/* Generated by the Python runner from Pipeline/liveview.c, not a copied implementation. */
#include FFC_WORKER_SOURCE

static void reset_capture(void)
{
    pull_count = release_count = held = open_count = 0;
    fail_pull = fail_open = fail_close = fail_map = 0;
    fail_release = 0;
    update_count = 0;
    post_open_reset_count = 0;
    queued_frames = queued_pulls = queue_on_transition = 0;
    expect_correction_lock = 0;
}

int main(void)
{
    size_t bytes = FFC_SHADING_PIXELS * sizeof(UINT16), i;
    UINT16 *scratch = calloc(1, bytes), *corrected = calloc(1, bytes), *b = calloc(1, bytes);
    UINT16 *average = calloc(1, bytes);
    AGC_WEIGHTED_WORKSPACE *ws = calloc(1, sizeof(*ws));
    VIDEO_LIVEVIEW stream = {0};
    UINT64 n = 0, t = 0;
    sensor_raw = calloc(1, bytes);
    assert(scratch && corrected && b && average && ws && sensor_raw);
    stream.thermal_ffc_out_va = (UINTPTR)scratch;
    stream.thermal_ffc_out_pa = (UINTPTR)scratch;
    stream.thermal_nuc_out_pa = (UINTPTR)corrected;
    stream.agc_weighted_workspace = ws;
    ffc_bind_runtime((UINTPTR)b, (UINT32)bytes); ffc_init_table();
    ordinary_average_test = 1; enabled_nuc = 0;
    for (i = 0; i < 2; ++i) {
        reset_capture();
        /* Dirty buffers also exercise resetting a previous FFC reference. */
        memset(scratch, 0x55, bytes);
        assert(liveview_capture_ffc_average(&stream, &n, &t, 1, 0, NULL) == 0);
        assert(pull_count == 4 && release_count == 4 && !held);
        assert((INT16)b[0] == 250 - 6458 && (INT16)b[1] == 16383 - 6458);
        assert((INT16)b[2] == -6458 && (INT16)b[3] == 9201 - 6458);
    }
    puts("PASS: actual four-frame CPU worker average 250/16383/0/9201, table and repeat reset");
    ordinary_average_test = 0; wide_average_test = 1;
    reset_capture();
    assert(liveview_capture_ffc_average(&stream, &n, &t, 1, 0, NULL) == 0);
    assert(pull_count == 4 && release_count == 4 && !held);
    wide_average_test = 0;
    puts("PASS: ordinary FFC UINT32 average preserves 20000 and 65535 without overflow");

    ffc_set_shutter_interval_sec(0);
    assert(!liveview_ffc_needs_refresh());
    ++nuc_epoch;
    /* Enable NUC: invalidate immediately, then rebuild even with timer off. */
    enabled_nuc = 1;
    assert(liveview_ffc_needs_refresh() && !ffc_table_is_ready());
    ffc_set_enabled(0); assert(!liveview_ffc_needs_refresh());
    ffc_set_enabled(1); assert(liveview_ffc_needs_refresh());
    enabled_nuc = 0;

    /* Real cycle drains old queued scenes and fresh transition frames on
     * both close and reopen; interval zero still permits a forced rebuild. */
    reset_capture(); queue_on_transition = 1; expect_correction_lock = 1;
    ffc_set_shutter_interval_sec(0);
    assert(liveview_run_shutter_calibration_cycle(&stream, &n, &t, 0, 1) == 0);
    assert(pull_count == 12 && queued_pulls == 4 && release_count == 16 && !held);
    assert(!shutter_closed && update_count == 1);
    assert(post_open_reset_count == 1);
    assert(ffc_table_is_ready() && !liveview_ffc_needs_refresh());
    assert((INT16)b[0] == 10 && (INT16)b[FFC_SHADING_PIXELS - 1] == -10);
    assert(!pthread_mutex_trylock(&g_manual_calibration_lock));
    pthread_mutex_unlock(&g_manual_calibration_lock);
    puts("PASS: shutter cycle starts AGC history reset only after reopen and fresh-frame drain");

    /* A capture failure reopens and unlocks without publishing a partial table. */
    reset_capture(); fail_pull = 6; expect_correction_lock = 1;
    assert(liveview_run_shutter_calibration_cycle(&stream, &n, &t, 1, 0) == -1);
    assert(!held && !shutter_closed);
    assert(post_open_reset_count == 0);
    assert((INT16)b[0] == 10 && (INT16)b[FFC_SHADING_PIXELS - 1] == -10);
    assert(!pthread_mutex_trylock(&g_manual_calibration_lock));
    pthread_mutex_unlock(&g_manual_calibration_lock);
    reset_capture(); fail_pull = 10; /* Fresh-frame drain after reopen fails. */
    assert(liveview_run_shutter_calibration_cycle(&stream, &n, &t, 1, 0) == -1);
    assert(!held && !shutter_closed && post_open_reset_count == 0);
    reset_capture(); fail_open = 1;
    assert(liveview_run_shutter_calibration_cycle(&stream, &n, &t, 1, 0) == -1);
    assert(!held && shutter_closed && post_open_reset_count == 0);
    assert(gpio_shutter_open() == 0);
    {
        UINT32 epoch = ffc_table_generation();
        reset_capture(); fail_release = 4;
        assert(liveview_capture_ffc_average(&stream, &n, &t, 1, 0, NULL) == -1);
        assert(ffc_table_generation() == epoch);
        assert((INT16)b[0] == 10 && (INT16)b[FFC_SHADING_PIXELS - 1] == -10);
    }
    puts("PASS: partial capture and final release failure preserve the previous reference");
    ordinary_average_test = 0; enabled_nuc = 1; reset_count = 0;
    reset_capture();
    assert(liveview_run_shading_calibration(&stream, &n, &t) == 0);
    assert(!shutter_closed && pull_count == 76 && release_count == 76 && !held);
    assert(update_count == 1 && reset_count == 1 && ffc_shading_is_ready());
    assert(g_shading_correction[0] == -10 && g_shading_correction[FFC_SHADING_PIXELS - 1] == 10);
    ffc_set_shading_enabled(1); ffc_refresh_shading();
    for (i = 0; i < FFC_SHADING_PIXELS; ++i) assert(b[i] == 0);
    puts("PASS: worker 32+32 post-NUC captures, frozen upstream tables, reopen and fresh FFC base");
    reset_capture(); scene_level = 60000;
    assert(liveview_capture_ffc_average(&stream, &n, &t, 1, 0, average) == 0);
    assert(pull_count == 32 && release_count == 32);
    for (i = 0; i < FFC_SHADING_PIXELS; ++i) assert(average[i] == 60501);
    puts("PASS: CPU 32-frame accumulation avoids 16-bit sum overflow and includes NUC output");
    scene_level = 12000;
    reset_capture(); enabled_nuc = 0;
    assert(liveview_run_shading_calibration(&stream, &n, &t) == -1 && open_count == 0);
    enabled_nuc = 1; dynamic_nuc = 1;
    assert(liveview_run_shading_calibration(&stream, &n, &t) == -1 && open_count == 0);
    dynamic_nuc = 0;
    reset_capture(); fail_pull = 41;
    assert(liveview_run_shading_calibration(&stream, &n, &t) == -1);
    assert(!shutter_closed && !held && release_count == 44 && ffc_shading_is_ready());
    reset_capture(); fail_map = 1;
    assert(liveview_run_shading_calibration(&stream, &n, &t) == -1);
    assert(!shutter_closed && !held && release_count == 9);
    reset_capture(); fail_close = 1;
    assert(liveview_run_shading_calibration(&stream, &n, &t) == -1 && !shutter_closed && !held);
    reset_capture(); fail_open = 2;
    assert(liveview_run_shading_calibration(&stream, &n, &t) == -1);
    assert(!shutter_closed && open_count == 3 && g_shading_correction[0] == -10);
    puts("PASS: invalid NUC, capture/map/shutter failures release frames, reopen and retain old calibration");
    ffc_bind_runtime(0, 0);
    free(scratch); free(corrected); free(b); free(average); free(ws); free(sensor_raw);
    return 0;
}
