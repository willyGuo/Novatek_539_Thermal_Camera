"""Exercise actual production RAW wrapper/stage with HDAL mocks and SDK types."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from raw_denoise_sources import RAW_DENOISE_SOURCES

parser = argparse.ArgumentParser()
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
cc = shutil.which(args.cc); assert cc
env = os.environ.copy(); env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
pipeline = (root/'Pipeline/liveview.c').read_text(encoding='utf-8')
helper_start = pipeline.index('typedef struct {\n\tUINTPTR base_va, data_va;')
helper = pipeline[helper_start:pipeline.index('static int liveview_update_ffc_table_from_average(')]
assert 'RAW temporal denoise (CPU): threshold=' not in helper
assert 'raw_temporal_total_us' not in pipeline
assert 'raw_nlm' not in pipeline and 'thermal_nlm' not in pipeline
stage = pipeline[pipeline.index('    /* Software RAW temporal denoise:'):pipeline.index('\t/* Corrected RAW snapshots/recordings')]
code = r'''
#include "hdal.h"
#include "Enhancement/RAW/raw_temporal_denoise.h"
#include "Config/hardware_config.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RAW_SIZE_W THERMAL_SENSOR_WIDTH
#define RAW_SIZE_H THERMAL_SENSOR_HEIGHT
typedef struct {
    RAW_TEMPORAL_DENOISE_STATE raw_temporal;
    UINT32 raw_temporal_tag, raw_temporal_calibration, raw_temporal_shading, raw_temporal_ffc_generation;
    UINT32 raw_temporal_shutter_generation;
    int raw_temporal_input_reported;
    HD_COMMON_MEM_VB_BLK thermal_raw_temporal_blk;
    UINTPTR thermal_raw_temporal_pa, thermal_raw_temporal_va;
    int cap_exit, proc_exit;
} VIDEO_LIVEVIEW;
static pthread_mutex_t g_manual_calibration_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_manual_calibration_active, cancelled, stopped, map_fail, map_calls, unmap_calls, flush_fail;
static UINTPTR input_base_va;
static UINT32 input_bytes;
static UINT32 g_weighted_calibration_generation, ffc_generation, shading;
static UINT32 g_manual_shutter_open_generation;
static int tdc_is_enabled(void) { return 0; }
static int ffc_is_enabled(void) { return 1; }
static int ffc_table_is_ready(void) { return 1; }
static int bpr_is_enabled(void) { return 1; }
static UINT32 ffc_shading_generation(void) { return shading; }
static UINT32 ffc_table_generation(void) { return ffc_generation; }
static void snapshot_writer_cancel_request(void) { ++cancelled; }
static void liveview_request_fault_stop(VIDEO_LIVEVIEW *s, int n) { (void)s;(void)n;++stopped; }
HD_RESULT hd_common_mem_flush_cache(void *va, unsigned int size)
{ assert(va && size); return flush_fail && (UINTPTR)va != input_base_va ? HD_ERR_NG : HD_OK; }
UINTPTR hd_common_mem_blk2pa(HD_COMMON_MEM_VB_BLK blk) { assert(blk==321); return input_base_va; }
void *hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE type, UINTPTR pa, UINT32 size)
{
    assert(type==HD_COMMON_MEM_MEM_TYPE_CACHE && pa==input_base_va && size==input_bytes);
    ++map_calls;
    return map_fail ? NULL : (void *)pa;
}
HD_RESULT hd_common_mem_munmap(void *va, unsigned int size)
{ assert((UINTPTR)va==input_base_va && size==input_bytes); ++unmap_calls; return HD_OK; }
'''
code += helper
code += r'''
static int run(VIDEO_LIVEVIEW *p_stream0, HD_VIDEO_FRAME *source, int isp_step_test_requested, int snapshot_requested) {
    HD_VIDEO_FRAME raw_temporal_video_frame = {0};
    HD_VIDEO_FRAME *current_frame = source;
    int current_frame_is_vcap = 1, nuc_applied = 1;
'''
code += stage
code += r'''
    assert(!pthread_mutex_trylock(&g_manual_calibration_lock)); pthread_mutex_unlock(&g_manual_calibration_lock);
    if (!current_frame_is_vcap) {
        assert(current_frame->phy_addr[0] == p_stream0->thermal_raw_temporal_pa);
        assert(current_frame->timestamp == source->timestamp && current_frame->count == source->count);
    }
    return ((UINT16 *)current_frame->phy_addr[0])[0];
release_vcap_frame:
    assert(!pthread_mutex_trylock(&g_manual_calibration_lock)); pthread_mutex_unlock(&g_manual_calibration_lock);
    return -1;
}
int main(void) {
    VIDEO_LIVEVIEW s = {0}; HD_VIDEO_FRAME f = {0};
    RAW_TEMPORAL_DENOISE_PARAMS p = {.enable=0,.threshold_dn=12,.history_shift=2};
    const unsigned pitch = RAW_SIZE_W + 8U;
    size_t n = (size_t)RAW_SIZE_W * RAW_SIZE_H, i, y;
    UINT16 *storage = malloc((4 + (size_t)pitch * RAW_SIZE_H) * 2), *dest = malloc(n * 2);
    UINT16 *input;
    assert(storage && dest);
    input = storage + 4;
    input_base_va = (UINTPTR)storage;
    input_bytes = (UINT32)((4 + (size_t)pitch * RAW_SIZE_H) * 2);
    for(y=0;y<RAW_SIZE_H;++y) for(i=0;i<pitch;++i) input[y*pitch+i] = i<RAW_SIZE_W ? 9400 : 65535;
    s.thermal_raw_temporal_va = s.thermal_raw_temporal_pa = (UINTPTR)dest;
    s.thermal_raw_temporal_blk = 123;
    f.phy_addr[0] = (UINTPTR)input; f.blk = 321;
    f.pxlfmt = HD_VIDEO_PXLFMT_RAW16 | HD_VIDEO_PIX_PACK_LSB;
    f.dim.w = RAW_SIZE_W; f.dim.h = RAW_SIZE_H; f.loff[0] = pitch * 2U;
    f.timestamp = 1000; f.count = 5;
    assert(copy_videoframe_raw(&f,(UINTPTR)dest,(UINT32)(n*2))==0);
    assert(dest[n-1]==9400 && map_calls==1 && unmap_calls==1);
    map_calls=unmap_calls=0;
    assert(!raw_temporal_denoise_set_params(&p));
    assert(run(&s,&f,0,0)==9400 && !map_calls && !s.raw_temporal.history_q8);
    p.enable=1; assert(!raw_temporal_denoise_set_params(&p));
    assert(run(&s,&f,0,0)==9400 && dest[n-1]==9400 && map_calls==1 && unmap_calls==1);
    f.loff[0] = pitch; /* Some frames report the stride in pixels. */
    input[0]=9404; f.timestamp+=33000;
    assert(run(&s,&f,0,0)==9401 && input[0]==9404);
    ffc_generation++; input[0]=9408; f.timestamp+=33000;
    assert(run(&s,&f,0,0)==9408);
    g_manual_shutter_open_generation++;input[0]=9400;f.timestamp+=33000;
    assert(run(&s,&f,0,0)==9400);
    input[0]=9408;f.timestamp+=33000;
    assert(run(&s,&f,0,0)==9402); /* New scene history resumes normally. */
    shading++; input[0]=9400; f.timestamp+=33000;
    assert(run(&s,&f,0,0)==9400);
    g_weighted_calibration_generation++;input[0]=9408;f.timestamp+=33000;
    assert(run(&s,&f,0,0)==9408);
    g_manual_calibration_active=1;
    assert(run(&s,&f,0,0)==9408 && !s.raw_temporal.valid);
    g_manual_calibration_active=0;
    assert(run(&s,&f,1,0)==9408 && !s.raw_temporal.valid);
    assert(run(&s,&f,0,0)==9408);
    input[0]=20000; f.timestamp+=33000;
    assert(run(&s,&f,0,0)==20000 && input[0]==20000);
    input[0]=20004; f.timestamp+=33000;
    assert(run(&s,&f,0,0)==20001 && input[0]==20004);
    input[0]=65535; f.timestamp+=33000;
    assert(run(&s,&f,0,0)==65535 && input[0]==65535);
    input[0]=65531; f.timestamp+=33000;
    assert(run(&s,&f,0,0)==65534 && input[0]==65531);
    p=(RAW_TEMPORAL_DENOISE_PARAMS)RAW_TEMPORAL_DENOISE_DEFAULTS;
    assert(!p.enable);p.enable=1;
    assert(!raw_temporal_denoise_set_params(&p));
    for(y=0;y<RAW_SIZE_H;++y) for(i=0;i<RAW_SIZE_W;++i) input[y*pitch+i]=1000;
    f.timestamp+=33000;assert(run(&s,&f,0,0)==1000);
    for(y=0;y<RAW_SIZE_H;++y) for(i=0;i<RAW_SIZE_W;++i) input[y*pitch+i]=1004;
    f.timestamp+=33000;assert(run(&s,&f,0,0)==1000 && input[0]==1004);
    for(y=0;y<RAW_SIZE_H;++y) for(i=0;i<RAW_SIZE_W;++i) input[y*pitch+i]=1008;
    f.timestamp+=33000;assert(run(&s,&f,0,0)==1005 && input[0]==1008);
    for(y=0;y<RAW_SIZE_H;++y) for(i=0;i<RAW_SIZE_W;++i) input[y*pitch+i]=1020;
    f.timestamp+=33000;assert(run(&s,&f,0,0)==1020 && input[0]==1020);
    p.enable=0;assert(!raw_temporal_denoise_set_params(&p));
    {int before=map_calls;assert(run(&s,&f,0,0)==1020 && before==map_calls);}
    p.enable=1;assert(!raw_temporal_denoise_set_params(&p));
    assert(dest[n-1]==1020 && input[RAW_SIZE_W]==65535 && map_calls==unmap_calls);
    puts("PASS: mapped Spatial/Dual Slow/Fast/RAW, padded source unchanged, master OFF skips processing");
    assert(!stopped && !cancelled);
    puts("PASS: corrected values above 16383 preserve DN/precision and continue to AGC");
    map_fail=1;
    assert(run(&s,&f,0,1)==-1 && cancelled==1 && stopped==1 && s.cap_exit && s.proc_exit);
    map_fail=0; f.pxlfmt=HD_VIDEO_PXLFMT_RAW12;
    assert(run(&s,&f,0,0)==-1);
    f.pxlfmt=HD_VIDEO_PXLFMT_RAW16;
    assert(run(&s,&f,0,0)==-1); /* Missing LSB flag stays a format error. */
    f.pxlfmt=HD_VIDEO_PXLFMT_RAW16 | HD_VIDEO_PIX_PACK_LSB;
    flush_fail=1;
    assert(run(&s,&f,0,0)==-1);
    flush_fail=0; s.thermal_raw_temporal_blk=HD_COMMON_MEM_VB_INVALID_BLK;
    assert(run(&s,&f,0,0)==-1);
    assert(map_calls==unmap_calls+1); /* Only the failed mmap has no matching unmap. */
    raw_temporal_denoise_destroy(&s.raw_temporal);free(storage);free(dest);
    puts("PASS: production temporal wrapper: corrected UINT16, bypass/reset, metadata, format/map/cache/buffer diagnostics and cleanup");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='raw_temporal_stage_') as tmp:
    tmp = Path(tmp); src = tmp/'stage.c';src.write_text(code,encoding='utf-8')
    for profile in (1,2):
        exe=tmp/f'test_{profile}.exe'
        cmd=[cc,'-std=gnu99','-O2','-Wall','-Wextra','-Werror','-D__LINUX',f'-DTHERMAL_SENSOR={profile}','-I',str(root)]
        for inc in ['hdal/include','vos/drivers/include']:
            cmd+=['-I',str(args.sdk/inc)]
        subprocess.run(cmd+[str(src),*RAW_DENOISE_SOURCES,'-pthread','-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)
order=pipeline[pipeline.index('static void liveview_process_live_frame('):]
assert order.index('ffc_apply(') < order.index('liveview_apply_raw_temporal(') < order.index('snapshot_final_frame = current_frame') < order.index('liveview_apply_agc_weighted(')
assert 'raw_temporal_denoise_destroy(&p_stream->raw_temporal)' in pipeline
assert 'mem_cfg.pool_info[id].blk_cnt = 2 + 12;' in pipeline
print('PASS: production placement and dedicated RAW pool/cleanup integration')
