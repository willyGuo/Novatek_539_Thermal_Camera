"""Exercise actual production RAW wrapper/stage with HDAL mocks and SDK types."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
cc = shutil.which(args.cc); assert cc
env = os.environ.copy(); env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
pipeline = (root/'Pipeline/liveview.c').read_text(encoding='utf-8')
helper = pipeline[pipeline.index('static int liveview_apply_raw_temporal('):pipeline.index('static int liveview_update_ffc_table_from_average(')]
assert 'RAW temporal denoise (CPU): threshold=' not in helper
assert 'raw_temporal_total_us' not in pipeline
assert 'raw_nlm' not in pipeline and 'thermal_nlm' not in pipeline
stage = pipeline[pipeline.index('    /* Software RAW temporal denoise:'):pipeline.index('\t/* Corrected RAW snapshots/recordings')]
code = r'''
#include "hdal.h"
#include "Enhancement/RAW/raw_temporal_denoise.h"
#include "Config/hardware_config.h"
#include <assert.h>
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
static int g_manual_calibration_active, cancelled, stopped, copy_fail, copy_calls, flush_fail;
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
HD_RESULT hd_common_mem_flush_cache(void *va, unsigned int size) { assert(va && size); return flush_fail ? HD_ERR_NG : HD_OK; }
static int copy_videoframe_raw(HD_VIDEO_FRAME *f, UINTPTR va, UINT32 size)
{ ++copy_calls; if (copy_fail) return -1; memcpy((void *)va, (void *)f->phy_addr[0], size); return 0; }
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
    RAW_TEMPORAL_DENOISE_PARAMS p = {0,12,2};
    size_t n = (size_t)RAW_SIZE_W * RAW_SIZE_H, i;
    UINT16 *input = malloc(n * 2), *dest = malloc(n * 2);
    assert(input && dest);
    for(i=0;i<n;++i) input[i] = 9400;
    s.thermal_raw_temporal_va = s.thermal_raw_temporal_pa = (UINTPTR)dest;
    s.thermal_raw_temporal_blk = 123;
    f.phy_addr[0] = (UINTPTR)input; f.blk = 321;
    f.pxlfmt = HD_VIDEO_PXLFMT_RAW16 | HD_VIDEO_PIX_PACK_LSB;
    f.dim.w = RAW_SIZE_W; f.dim.h = RAW_SIZE_H; f.timestamp = 1000; f.count = 5;
    assert(!raw_temporal_denoise_set_params(&p));
    assert(run(&s,&f,0,0)==9400 && !copy_calls && !s.raw_temporal.history_q8);
    p.enable=1; assert(!raw_temporal_denoise_set_params(&p));
    assert(run(&s,&f,0,0)==9400 && copy_calls==1);
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
    assert(!stopped && !cancelled);
    puts("PASS: corrected values above 16383 preserve DN/precision and continue to AGC");
    copy_fail=1;
    assert(run(&s,&f,0,1)==-1 && cancelled==1 && stopped==1 && s.cap_exit && s.proc_exit);
    copy_fail=0; f.pxlfmt=HD_VIDEO_PXLFMT_RAW12;
    assert(run(&s,&f,0,0)==-1);
    f.pxlfmt=HD_VIDEO_PXLFMT_RAW16;
    assert(run(&s,&f,0,0)==-1); /* Missing LSB flag stays a format error. */
    f.pxlfmt=HD_VIDEO_PXLFMT_RAW16 | HD_VIDEO_PIX_PACK_LSB;
    flush_fail=1;
    assert(run(&s,&f,0,0)==-1);
    flush_fail=0; s.thermal_raw_temporal_blk=HD_COMMON_MEM_VB_INVALID_BLK;
    assert(run(&s,&f,0,0)==-1);
    raw_temporal_denoise_destroy(&s.raw_temporal);free(input);free(dest);
    puts("PASS: production temporal wrapper: native corrected UINT16, bypass/reset, metadata, format/copy/cache/buffer diagnostics and cleanup");
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
        subprocess.run(cmd+[str(src),str(root/'Enhancement/RAW/raw_temporal_denoise.c'),'-pthread','-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)
order=pipeline[pipeline.index('static void liveview_process_live_frame('):]
assert order.index('ffc_apply(') < order.index('liveview_apply_raw_temporal(') < order.index('snapshot_final_frame = current_frame') < order.index('liveview_apply_agc_weighted(')
assert 'raw_temporal_denoise_destroy(&p_stream->raw_temporal)' in pipeline
assert 'mem_cfg.pool_info[id].blk_cnt = 2 + 12;' in pipeline
print('PASS: production placement and dedicated RAW pool/cleanup integration')
