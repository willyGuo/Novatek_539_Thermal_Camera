"""Execute extracted production AGC switch/RAW wrapper with mocked HDAL.

Uses real AGC modes and algorithm modules; no SDK linking or board proof.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = shutil.which('gcc')
assert cc
env = os.environ.copy()
env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
source = (root / 'Pipeline/liveview.c').read_text(encoding='utf-8')


def section(start, end):
    a = source.index(start)
    return source[a:source.index(end, a)]


code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include "agc_mode.h"
#include "agc_weighted.h"
#include "agc_fieldscale.h"
#include "agc_histogram_p1_p99.h"
typedef uintptr_t UINTPTR;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int HD_RESULT;
#define RAW_SIZE_W 640U
#define FRAME_DIAGNOSTICS_ENABLED 0
#define HD_OK 0
#define HD_COMMON_MEM_MEM_TYPE_CACHE 0
#define IQT_ITEM_TONE_PARAM 1
typedef struct {
    struct { uint32_t w,h; } dim;
    uintptr_t phy_addr[3],blk;
    uint32_t loff[3],ph[3];
    uint64_t count;
} HD_VIDEO_FRAME;
typedef struct { int id;struct { uint32_t enable; } tone; } IQT_TONE_PARAM;
typedef struct {
    AGC_WEIGHTED_WORKSPACE *agc_weighted_workspace;
    uint32_t *agc_histogram_p1_p99_workspace;
    void *agc_fieldscale_horizontal_workspace;
    AGC_HISTOGRAM_P1_P99_STATE agc_histogram_p1_p99_state;
    AGC_FIELDSCALE_STATE agc_fieldscale_state;
    uintptr_t thermal_agc_histogram_p1_p99_out_pa,thermal_agc_histogram_p1_p99_out_va;
    uintptr_t thermal_agc_histogram_p1_p99_out_blk;
    uint32_t agc_weighted_shading_generation,agc_weighted_calibration_generation;
    uint32_t agc_weighted_shutter_generation;
    uint32_t agc_weighted_frame_count;
} VIDEO_LIVEVIEW;
static IQT_TONE_PARAM g_agc_baseline_tone={0,{1}};
static int g_agc_baseline_valid=1,fail_iq,tone_calls,flush_calls,map_calls,unmap_calls;
static uint32_t applied_tone=99,shading_generation,g_weighted_calibration_generation;
static uint32_t g_manual_shutter_open_generation;
static pthread_mutex_t g_manual_calibration_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_shutter_calibration_lock=PTHREAD_MUTEX_INITIALIZER;
static int mock_shutter_closed,fail_open,fail_close;
static int gpio_shutter_open(void) {
    assert(pthread_mutex_trylock(&g_manual_calibration_lock)!=0);
    if(fail_open)return -1;
    mock_shutter_closed=0;return 0;
}
static int gpio_shutter_close(void) {
    assert(pthread_mutex_trylock(&g_manual_calibration_lock)!=0);
    if(fail_close)return -1;
    mock_shutter_closed=1;return 0;
}
static void liveview_set_shutter_fault(int fault) { assert(fault==0); }
static int vendor_isp_set_iq(int item,IQT_TONE_PARAM *p) {
    assert(item==IQT_ITEM_TONE_PARAM);++tone_calls;
    if(fail_iq)return -1;
    applied_tone=p->tone.enable;return 0;
}
static uintptr_t hd_common_mem_blk2pa(uintptr_t b) { return b; }
static void *hd_common_mem_mmap(int type,uintptr_t p,uint32_t size) {
    (void)type;assert(size>0);++map_calls;return (void *)p;
}
static void hd_common_mem_flush_cache(void *p,uint32_t n) { assert(p&&n);++flush_calls; }
static int hd_common_mem_munmap(void *p,uint32_t n) { assert(p&&n);++unmap_calls;return 0; }
static uint32_t ffc_shading_generation(void) { return shading_generation; }
void agc_histogram_p1_p99_state_reset(AGC_HISTOGRAM_P1_P99_STATE *p) { p->temporal_valid=0; }
void agc_fieldscale_state_reset(AGC_FIELDSCALE_STATE *p) { p->temporal_valid=0; }
int agc_histogram_p1_p99_menu_run(void) { return 0; }
int agc_fieldscale_menu_run(void) { return 0; }
'''
code += section('static int liveview_manual_shutter_command(', 'static void liveview_on_manual_calibration_state(')
code += section('static int liveview_apply_agc_weighted(', 'static int liveview_apply_agc_fieldscale(')
code += section('static void liveview_apply_agc_request(', '/* Each RAW input must complete')
code += r'''
int main(void) {
    VIDEO_LIVEVIEW s={0};HD_VIDEO_FRAME in={0},out={0};
    AGC_WEIGHTED_WORKSPACE *shared=calloc(1,sizeof(*shared)),*expected_shared=calloc(1,sizeof(*shared));
    AGC_WEIGHTED_PARAMS p;size_t n=(size_t)RAW_SIZE_W*RAW_SIZE_H,i;
    uint16_t *raw=malloc(n*2),*dest=malloc(n*2),*expected=malloc(n*2);
    uint32_t dummy=0;int calls;
    assert(shared&&expected_shared&&raw&&dest&&expected);
    s.agc_weighted_workspace=shared;
    s.agc_histogram_p1_p99_workspace=&dummy;s.agc_fieldscale_horizontal_workspace=&dummy;
    s.thermal_agc_histogram_p1_p99_out_va=(uintptr_t)dest;
    s.thermal_agc_histogram_p1_p99_out_pa=(uintptr_t)dest;
    s.thermal_agc_histogram_p1_p99_out_blk=777;
    assert(agc_mode_set(AGC_MODE_OFF)==0);
    assert(agc_mode_set((AGC_MODE)2)==-1 && agc_mode_request((AGC_MODE)2)==-1);
    assert(agc_mode_set((AGC_MODE)5)==-1 && agc_mode_request((AGC_MODE)5)==-1);
    assert(agc_mode_set((AGC_MODE)99)==-1 && agc_mode_request((AGC_MODE)99)==-1);
    agc_mode_begin_session();assert(agc_mode_take_request()==-1);
    calls=tone_calls;s.agc_weighted_workspace=NULL;
    assert(agc_mode_request(AGC_MODE_WEIGHTED_CPU)==0);liveview_apply_agc_request(&s);
    assert(agc_mode_get()==AGC_MODE_OFF && tone_calls==calls);
    s.agc_weighted_workspace=shared;g_agc_baseline_valid=0;
    agc_mode_request(AGC_MODE_WEIGHTED_CPU);liveview_apply_agc_request(&s);
    assert(agc_mode_get()==AGC_MODE_OFF && tone_calls==calls);
    g_agc_baseline_valid=1;fail_iq=1;shared->temporal_valid=1;
    agc_mode_request(AGC_MODE_WEIGHTED_CPU);liveview_apply_agc_request(&s);
    assert(agc_mode_get()==AGC_MODE_OFF && shared->temporal_valid);
    fail_iq=0;
    agc_mode_request(AGC_MODE_OFF);agc_mode_request(AGC_MODE_WEIGHTED_CPU);
    liveview_apply_agc_request(&s);
    assert(agc_mode_get()==AGC_MODE_WEIGHTED_CPU && applied_tone==0);
    assert(!shared->temporal_valid&&agc_mode_take_request()==-1);
    assert(strstr(agc_mode_name(agc_mode_get()),"Weighted"));
    agc_weighted_default_params(&p);assert(agc_weighted_set_params(&p)==0);
    for(i=0;i<n;i++)raw[i]=(uint16_t)(4500+(i%640)/16+(i%7));
    in.dim.w=RAW_SIZE_W;in.dim.h=RAW_SIZE_H;in.blk=(uintptr_t)raw;
    in.phy_addr[0]=(uintptr_t)raw;in.loff[0]=RAW_SIZE_W*2;in.ph[0]=RAW_SIZE_H;in.count=123;
    assert(liveview_apply_agc_weighted(&s,&in,&out)==0);
    assert(agc_weighted_apply_u14(raw,RAW_SIZE_W*2,expected,RAW_SIZE_W*2,
        RAW_SIZE_W,RAW_SIZE_H,expected_shared,&p)==0);
    assert(memcmp(dest,expected,n*2)==0 && out.phy_addr[0]==(uintptr_t)dest && out.blk==777);
    assert(out.count==in.count&&out.loff[0]==RAW_SIZE_W*2&&out.ph[0]==RAW_SIZE_H);
    assert(map_calls==1&&flush_calls==2&&unmap_calls==1);
    /* New branch uses the same production RAW frame dispatch and history resets. */
    p.balanced_background=1;assert(agc_weighted_set_params(&p)==0);
    assert(liveview_apply_agc_weighted(&s,&in,&out)==0);
    agc_weighted_state_reset(expected_shared);
    assert(agc_weighted_apply_u14(raw,RAW_SIZE_W*2,expected,RAW_SIZE_W*2,
        RAW_SIZE_W,RAW_SIZE_H,expected_shared,&p)==0);
    assert(memcmp(dest,expected,n*2)==0);
    /* Actual wrapper resets the new history after calibration or shading changes. */
    p.temporal_previous_percent=90;agc_weighted_set_params(&p);
    assert(liveview_apply_agc_weighted(&s,&in,&out)==0);
    for(i=0;i<n;i++)raw[i]+=35;
    assert(liveview_apply_agc_weighted(&s,&in,&out)==0);
    ++g_weighted_calibration_generation;
    assert(liveview_apply_agc_weighted(&s,&in,&out)==0);
    assert(shared->temporal_valid);
    agc_weighted_state_reset(expected_shared);
    agc_weighted_apply_u14(raw,RAW_SIZE_W*2,expected,RAW_SIZE_W*2,RAW_SIZE_W,RAW_SIZE_H,expected_shared,&p);
    assert(memcmp(dest,expected,n*2)==0);
    for(i=0;i<n;i++)raw[i]+=17;
    ++shading_generation;
    assert(liveview_apply_agc_weighted(&s,&in,&out)==0);
    assert(shared->temporal_valid);
    agc_weighted_state_reset(expected_shared);
    agc_weighted_apply_u14(raw,RAW_SIZE_W*2,expected,RAW_SIZE_W*2,RAW_SIZE_W,RAW_SIZE_H,expected_shared,&p);
    assert(memcmp(dest,expected,n*2)==0);
    /* Manual open notifies the RAW worker without modifying its workspace
     * on the UI thread, changing calibration generation, or running FFC. */
    assert(liveview_manual_shutter_command(0)==0 && mock_shutter_closed);
    assert(g_manual_shutter_open_generation==0);
    for(i=0;i<n;i++)raw[i]+=350;
    assert(liveview_manual_shutter_command(1)==0 && !mock_shutter_closed);
    assert(g_manual_shutter_open_generation==1 && g_weighted_calibration_generation==1);
    assert(s.agc_weighted_shutter_generation==0);
    assert(liveview_apply_agc_weighted(&s,&in,&out)==0);
    assert(s.agc_weighted_shutter_generation==1 && shared->temporal_valid);
    agc_weighted_state_reset(expected_shared);
    agc_weighted_apply_u14(raw,RAW_SIZE_W*2,expected,RAW_SIZE_W*2,RAW_SIZE_W,RAW_SIZE_H,expected_shared,&p);
    assert(memcmp(dest,expected,n*2)==0);
    assert(liveview_apply_agc_weighted(&s,&in,&out)==0 && shared->temporal_valid);
    fail_open=1;assert(liveview_manual_shutter_command(1)==-1);
    assert(g_manual_shutter_open_generation==1);fail_open=0;
    fail_close=1;assert(liveview_manual_shutter_command(0)==-1);fail_close=0;
    pthread_mutex_lock(&g_shutter_calibration_lock);
    assert(liveview_manual_shutter_command(1)==-1 && g_manual_shutter_open_generation==1);
    pthread_mutex_unlock(&g_shutter_calibration_lock);
    agc_mode_request(AGC_MODE_WEIGHTED_CPU);liveview_apply_agc_request(&s);
    assert(agc_mode_get()==AGC_MODE_WEIGHTED_CPU&&shared->temporal_valid&&applied_tone==0);
    agc_mode_request(AGC_MODE_OFF);liveview_apply_agc_request(&s);
    assert(agc_mode_get()==AGC_MODE_OFF&&applied_tone==1);
    free(raw);free(dest);free(expected);free(shared);free(expected_shared);
    puts("PASS: actual AGC selection/request + RAW dispatch/cache/frame metadata, missing workspace/ISP failure rollback and calibration/shading resets");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='weighted_pipeline_') as tmp:
    work = Path(tmp)
    path = work / 'test.c'
    path.write_text(code, encoding='utf-8')
    for height in (480, 512):
        exe = work / f'test_{height}.exe'
        subprocess.run([cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
            f'-DRAW_SIZE_H={height}U', '-I', str(root / 'Enhancement/AGC'), str(path),
            *[str(root / ('Enhancement/AGC/' + name)) for name in (
                'agc_mode.c', 'agc_weighted.c')],
            '-pthread', '-lm', '-o', str(exe)], check=True, env=env)
        result = subprocess.run([str(exe)], check=False, env=env, capture_output=True, text=True)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        print(f'640x{height}: {result.stdout.splitlines()[-1]}')
assert 'mode > AGC_MODE_WEIGHTED_CPU' in source
assert 'free(p_stream->agc_weighted_workspace)' in source
assert 'sizeof(AGC_WEIGHTED_WORKSPACE)' in source
print('PASS: Weighted selection, preallocation and cleanup')
