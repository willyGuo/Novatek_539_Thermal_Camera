/* Host regression of real NUC table generation and GFX submission. */
#include <assert.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define mkdir(path, mode) _mkdir(path)
#define fsync(fd) _commit(fd)
#endif
#include "D:/01_Project/02_Loewe/03_Novatek/04_Develop/03_VM/ShareFolder/Tiffany_thermal/Correction/NUC/nuc.c"

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

int main(int argc,char **argv){
 size_t n=640U*480U; UINT16 *lo=malloc(n*2),*hi=malloc(n*2),*k=malloc(n*2),*b=malloc(n*2);FILE *f;UINT16 q14like=16384;
 assert(argc==5&&lo&&hi&&k&&b);
 f=fopen(argv[1],"rb");assert(f&&fread(lo,2,n,f)==n);fclose(f);
 f=fopen(argv[2],"rb");assert(f&&fread(hi,2,n,f)==n);fclose(f);
 g_nuc.raw_width=g_nuc.active_width=640;g_nuc.raw_height=g_nuc.active_height=480;g_nuc.table_size=n*2;
 assert(nuc_build_table(lo,hi,k,b)==0);
 f=fopen(argv[3],"wb");assert(f&&fwrite(k,2,n,f)==n);fclose(f);
 f=fopen(argv[4],"wb");assert(f&&fwrite(b,2,n,f)==n);fclose(f);
 printf("Q14 unity passes Q8 range check: %d\n",nuc_gain_table_validate_q8(&q14like,1)==0);
 free(lo);free(hi);free(k);free(b);return 0;}
