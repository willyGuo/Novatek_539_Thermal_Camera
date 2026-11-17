/* Host regression of real NUC table generation and GFX submission. */
#include <assert.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define mkdir(path, mode) _mkdir(path)
#define fsync(fd) _commit(fd)
#endif
#include "../Correction/NUC/nuc.c"

/* Unused platform dependencies are fail-fast stubs in this host test. */
HD_RESULT hd_common_mem_flush_cache(void *p, unsigned int n)
{ (void)p; (void)n; assert(0); return HD_ERR_NG; }
UINTPTR hd_common_mem_blk2pa(HD_COMMON_MEM_VB_BLK b)
{ (void)b; assert(0); return 0; }
void *hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE t, UINTPTR p, UINT32 n)
{ (void)t; (void)p; (void)n; assert(0); return NULL; }
HD_RESULT hd_common_mem_munmap(void *p, unsigned int n)
{ (void)p; (void)n; assert(0); return HD_ERR_NG; }
int sensor_temp_read(float *t)
{ (void)t; assert(0); return -1; }
int app_config_update_int(const char *k, int v)
{ (void)k; (void)v; assert(0); return -1; }
int app_config_update_sensor_temp_points(const float *p, unsigned int n)
{ (void)p; (void)n; assert(0); return -1; }
int bpr_load_table(void) { assert(0); return -1; }
int bpr_is_enabled(void) { assert(0); return 0; }
unsigned int bpr_get_threshold(void) { assert(0); return 0; }
int liveview_request_nuc_snapshot(void) { assert(0); return -1; }

HD_RESULT vendor_gfx_grph_trigger(VENDOR_GFX_ENG_ID id,
    VENDOR_GFX_GRPH_TRIGGER_PARAM *p)
{
    (void)id;
    assert(p->command == VENDOR_GRPH_CMD_Non_uni_correction);
    assert(p->format == VENDOR_GFX_GRPH_FORMAT_16BITS);
    assert(p->property[0].data.property == 8);
    return HD_OK;
}

int main(int argc, char **argv)
{
    UINT16 low[] = {1000, 1000}, high[] = {1100, 1300};
    UINT16 k[2], b[2], valid[] = {1, 256, 512, 32767};
    UINT16 invalid[] = {256, 32768}, zero[] = {0};
    HD_VIDEO_FRAME input = {0}, output = {0};
    UINT16 loaded[4] = {0};
    FILE *fp;
    assert(argc == 2);
    assert(NUC_GAIN_SHIFT == 8 && NUC_GAIN_ONE == 256);
    assert(strcmp(NUC_SENSOR_TEMP_DIR, NUC_DIR "/sensor_temp") == 0);
    assert(strcmp(NUC_BLACKBODY_TEMP_DIR, NUC_DIR "/blackbody_temp") == 0);
    assert(nuc_gain_table_validate_q8(valid, 4) == 0);
    assert(valid[1] == 256 && valid[2] == 512);
    assert(nuc_gain_table_validate_q8(invalid, 2) != 0);
    assert(nuc_gain_table_validate_q8(zero, 1) != 0);
    /* Native Q8 loads byte-for-byte, including gains above the old Q14 cap. */
    g_nuc.table_size = sizeof(valid);
    fp = fopen(argv[1], "wb");
    assert(fp && fwrite(valid, sizeof(valid), 1, fp) == 1);
    assert(fclose(fp) == 0);
    assert(nuc_read_gain_table(argv[1], loaded, 1) == 0);
    assert(memcmp(valid, loaded, sizeof(valid)) == 0);
    g_nuc.raw_width = g_nuc.active_width = 2;
    g_nuc.raw_height = g_nuc.active_height = 1;
    g_nuc.table_size = sizeof(k);
    assert(nuc_build_table(low, high, k, b) == 0);
    assert(k[0] == 512 && k[1] == 170);
    assert((INT16)b[0] == 500 && (INT16)b[1] == -506);
    assert(nuc_build_blackbody_point_table(low, k, b) == 0);
    assert(k[0] == 256 && k[1] == 256 && b[0] == 0 && b[1] == 0);
    /* Unsafe gain rejection must leave the previous tables intact. */
    high[0] = 1001; high[1] = 2000;
    assert(nuc_build_table(low, high, k, b) != 0);
    assert(k[0] == 256 && k[1] == 256);
    /* Legal RAW14 inputs can require an unrepresentable signed GFX B.
     * Reject before touching either table (previously silently clipped). */
    {
        UINT16 low4[] = {15000, 15000, 15000, 15000};
        UINT16 high4[] = {16000, 15010, 15010, 15010};
        UINT16 k4[] = {256, 257, 258, 259}, b4[] = {1, 2, 3, 4};
        UINT16 old_k[4], old_b[4];
        memcpy(old_k, k4, sizeof(k4)); memcpy(old_b, b4, sizeof(b4));
        g_nuc.raw_width = g_nuc.active_width = 4;
        g_nuc.table_size = sizeof(k4);
        assert(nuc_build_table(low4, high4, k4, b4) != 0);
        assert(!memcmp(k4, old_k, sizeof(k4)) && !memcmp(b4, old_b, sizeof(b4)));
    }
    {
        UINT32 epoch = nuc_correction_generation();
        g_nuc.table_loaded = 1;
        nuc_set_enabled(1); assert(nuc_correction_generation() != epoch);
        epoch = nuc_correction_generation();
        nuc_set_enabled(1); assert(nuc_correction_generation() == epoch);
        nuc_set_enabled(0); assert(nuc_correction_generation() != epoch);
    }
    assert(nuc_apply(&input, &output, 1, 2, 3, 4, 2, 1) == HD_OK);
    puts("NUC Q8 K/B rejection preserves both tables; correction epochs and GFX shift passed");
    return 0;
}
