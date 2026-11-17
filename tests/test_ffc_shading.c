/* Host-only regression: mocks GFX execution, not a board validation. */
#include <assert.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define mkdir(path, mode) _mkdir(path)
#define fsync(fd) _commit(fd)
int host_replace(const char *, const char *);
#define rename host_replace
#endif
#include "../Correction/FFC/ffc.c"

HD_RESULT hd_common_mem_flush_cache(void *addr, unsigned int size)
{
    (void)addr; (void)size;
    return HD_OK;
}

HD_RESULT vendor_gfx_grph_trigger(VENDOR_GFX_ENG_ID id,
    VENDOR_GFX_GRPH_TRIGGER_PARAM *p)
{
    size_t i, n = (size_t)p->images[0].width / 2 * p->images[0].height;
    UINT16 *x = (UINT16 *)p->images[0].dram_addr;
    UINT16 *k = (UINT16 *)p->images[1].dram_addr;
    UINT16 *y = (UINT16 *)p->images[2].dram_addr;
    INT16 *b = (INT16 *)p->images[3].dram_addr;
    (void)id;
    if (p->command == VENDOR_GFX_GRPH_CMD_PLUS_SHF) {
        const UINT16 *a = (const UINT16 *)p->images[0].dram_addr;
        const UINT16 *src_b = (const UINT16 *)p->images[1].dram_addr;
        unsigned shift = p->property[0].data.property & 3U;
        assert(p->format == VENDOR_GFX_GRPH_FORMAT_16BITS);
        for (i = 0; i < n; ++i) {
            UINT32 sum = a[i] + (src_b[i] >> shift);
            y[i] = (UINT16)(sum > 65535U ? 65535U : sum);
        }
        return HD_OK;
    }
    assert(p->command == VENDOR_GRPH_CMD_Non_uni_correction);
    assert(p->property[0].data.property == FFC_GAIN_SHIFT);
    for (i = 0; i < n; ++i)
        y[i] = ffc_clip_output(((long long)k[i] * ((long long)x[i] - b[i])) >> FFC_GAIN_SHIFT);
    return HD_OK;
}

int main(void)
{
    const size_t n = FFC_SHADING_PIXELS, bytes = n * sizeof(UINT16);
    UINT16 *o = malloc(bytes), *s = malloc(bytes), *b = malloc(bytes);
    UINT16 *k = malloc(bytes), *out = malloc(bytes);
    HD_VIDEO_FRAME in = {0}, output = {0};
    size_t i;
    FILE *fp;
    UINT32 generation;
    uint16_t small_o[] = {100, 100}, small_s[] = {101, 100};
    int16_t c[2];
    assert(o && s && b && k && out);
    assert(ffc_shading_build(small_o, small_s, c, 2) == 0 && c[0] == -1 && c[1] == 1);
    assert(ffc_shading_build(NULL, small_s, c, 2) == -1);
    assert(ffc_shading_build(small_o, small_s, c, 0) == -1);
    for (i = 0; i < n; ++i) {
        o[i] = 12000; /* Different blackbody DC level must not leak into C. */
        s[i] = i < n / 2 ? 10020 : 10000;
        k[i] = FFC_GAIN_ONE;
    }
    ffc_bind_runtime((UINTPTR)b, (UINT32)bytes);
    assert(ffc_init_table() == 0);
    assert(!ffc_is_shading_enabled() && !ffc_shading_is_ready());
    ffc_set_upstream_state(1, 0, 1, 1, 0);
    ffc_set_shading_input_mode(1, 0, 0);
    generation = ffc_table_generation();
    assert(ffc_update_table_from_raw((UINTPTR)s, THERMAL_SENSOR_WIDTH * 2,
        0, 0, THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT) == 0);
    assert(ffc_table_generation() != generation);
    assert((INT16)b[0] == 10 && (INT16)b[n - 1] == -10);
    assert(ffc_shading_calibrate(o, s, 0) == 0);
    assert(ffc_shading_is_ready());
    generation = ffc_shading_generation();
    ffc_set_shading_enabled(1);
    assert(ffc_shading_generation() != generation);
    in.phy_addr[0] = (UINTPTR)o;
    assert(ffc_apply(&in, &output, (UINTPTR)k, (UINTPTR)b,
        (UINTPTR)out, 0, THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT) == HD_OK);
    for (i = 0; i < n; ++i) assert(b[i] == 0 && out[i] == 12000);
    puts("PASS: sign, zero-mean normalization, GFX addresses/shift and uniform output");
    /* Repeated updates must not accumulate the fixed correction. */
    for (i = 0; i < 3; ++i) {
        assert(ffc_update_table_from_raw((UINTPTR)s, THERMAL_SENSOR_WIDTH * 2,
            0, 0, THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT) == 0);
        assert(b[0] == 0 && b[n - 1] == 0);
    }
    ffc_set_shading_enabled(0); ffc_refresh_shading();
    assert((INT16)b[0] == 10 && (INT16)b[n - 1] == -10);
    ffc_set_shading_enabled(1); ffc_refresh_shading();
    assert(b[0] == 0);
    ffc_set_shading_input_mode(0, 0, 0); ffc_refresh_shading(); assert((INT16)b[0] == 10);
    ffc_set_shading_input_mode(1, 1, 0); ffc_refresh_shading(); assert((INT16)b[0] == 10);
    ffc_set_shading_input_mode(1, 0, 1); ffc_refresh_shading(); assert((INT16)b[0] == 10);
    ffc_set_shading_input_mode(1, 0, 0); ffc_refresh_shading(); assert(b[0] == 0);
    puts("PASS: repeated FFC, immediate toggle and NUC/TDC mode gates");
    /* No-op checks preserve the table. Any changed upstream epoch/mode
     * invalidates it, and toggling FFC cannot resurrect the old reference. */
    generation = ffc_table_generation();
    ffc_set_upstream_state(1, 0, 1, 1, 0);
    assert(ffc_table_is_ready() && ffc_table_generation() == generation);
    {
        const UINT32 states[][5] = {
            {1, 0, 2, 1, 0}, /* NUC reload, same master enable */
            {0, 0, 2, 1, 0}, /* NUC disabled */
            {1, 0, 3, 1, 0}, /* NUC enabled */
            {1, 1, 3, 2, 0}, /* TDC enabled */
            {1, 1, 3, 3, 0}, /* TDC correction changed */
            {1, 1, 3, 3, 1}, /* Manual calibration completed */
        };
        size_t j;
        for (j = 0; j < sizeof(states) / sizeof(states[0]); ++j) {
            ffc_set_upstream_state(states[j][0], states[j][1],
                states[j][2], states[j][3], states[j][4]);
            assert(!ffc_table_is_ready() && ffc_refresh_is_required());
            assert(!ffc_shading_is_active());
            ffc_set_enabled(0); ffc_set_enabled(1);
            assert(!ffc_table_is_ready());
            assert(ffc_apply(&in, &output, (UINTPTR)k, (UINTPTR)b,
                (UINTPTR)out, 0, THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT) != HD_OK);
            assert(ffc_update_table_from_raw((UINTPTR)s, THERMAL_SENSOR_WIDTH * 2,
                0, 0, THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT) == 0);
            assert(ffc_table_is_ready() && !ffc_refresh_is_required());
        }
    }
    puts("PASS: upstream NUC/TDC/calibration changes block stale FFC until rebuild");
    /* Atomic replacement of an existing table and restart load. */
    assert(ffc_shading_calibrate(o, s, 0) == 0);
    ffc_bind_runtime(0, 0);
    ffc_bind_runtime((UINTPTR)b, (UINT32)bytes);
    assert(ffc_shading_is_ready());
    assert(ffc_init_table() == 0);
    assert(ffc_update_table_from_raw((UINTPTR)s, THERMAL_SENSOR_WIDTH * 2,
        0, 0, THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT) == 0);
    assert(b[0] == 0 && b[n - 1] == 0);
    /* Range or write failure must retain the last RAM and disk table. */
    for (i = 0; i < n; ++i) { o[i] = i < n / 2 ? 65535 : 0; s[i] = i < n / 2 ? 0 : 65535; }
    assert(ffc_shading_calibrate(o, s, 0) == -1 && ffc_shading_is_ready());
    assert(mkdir(FFC_SHADING_NEW, 0777) == 0);
    for (i = 0; i < n; ++i) { o[i] = 12000; s[i] = i < n / 2 ? 10020 : 10000; }
    assert(ffc_shading_calibrate(o, s, 0) == -1 && ffc_shading_is_ready());
    rmdir(FFC_SHADING_NEW);
    ffc_bind_runtime(0, 0); ffc_bind_runtime((UINTPTR)b, (UINT32)bytes);
    assert(ffc_shading_is_ready());
    puts("PASS: persistence, atomic replacement and failed-calibration rollback");
    /* Wrong profile and corrupt/truncated tables are refused. */
    fp = fopen(FFC_SHADING_PATH, "r+b"); assert(fp);
    assert(fseek(fp, 8, SEEK_SET) == 0); fputc(99, fp); fclose(fp);
    ffc_bind_runtime(0, 0); ffc_bind_runtime((UINTPTR)b, (UINT32)bytes);
    assert(!ffc_shading_is_ready());
    assert(ffc_shading_calibrate(o, s, 0) == 0);
    fp = fopen(FFC_SHADING_PATH, "r+b"); assert(fp);
    assert(fseek(fp, 32, SEEK_SET) == 0); fputc(99, fp); fclose(fp);
    ffc_bind_runtime(0, 0); ffc_bind_runtime((UINTPTR)b, (UINT32)bytes);
    assert(!ffc_shading_is_ready());
    fp = fopen(FFC_SHADING_PATH, "wb"); assert(fp); fputc(0, fp); fclose(fp);
    ffc_bind_runtime(0, 0); ffc_bind_runtime((UINTPTR)b, (UINT32)bytes);
    assert(!ffc_shading_is_ready());
    puts("PASS: profile, checksum and truncation rejection");
    ffc_bind_runtime(0, 0);
    free(o); free(s); free(b); free(k); free(out);
    return 0;
}
