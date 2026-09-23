"""Host regression for real nuc.c using mocked SDK I/O; no board validation.
Run: python tests/test_nuc_blackbody_segments.py [gcc-path]
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HDAL = r'''
#ifndef HDAL_H
#define HDAL_H
#include <stdint.h>
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTPTR;
typedef uintptr_t HD_COMMON_MEM_VB_BLK;
typedef int HD_RESULT;
#define HD_OK 0
#define HD_ERR_NG (-1)
#define HD_COMMON_MEM_MEM_TYPE_CACHE 0
typedef struct { UINT32 w,h; } DIM;
typedef struct { UINTPTR blk,phy_addr[1]; UINT32 loff[1],ph[1]; DIM dim; } HD_VIDEO_FRAME;
static inline UINTPTR hd_common_mem_blk2pa(UINTPTR p) { return p; }
static inline void *hd_common_mem_mmap(int t, UINTPTR p, UINT32 n) { (void)t; (void)n; return (void*)p; }
static inline int hd_common_mem_munmap(void *p, UINT32 n) { (void)p; (void)n; return 0; }
static inline void hd_common_mem_flush_cache(void *p, UINT32 n) { (void)p; (void)n; }
#endif
'''
GFX = r'''
#define VENDOR_GRPH_CMD_Non_uni_correction 0
#define VENDOR_GFX_GRPH_FORMAT_16BITS 0
#define VENDOR_GFX_GRPH_PROPERTY_ID_NORMAL 0
#define VENDOR_GFX_GRPH_IMG_ID_A 0
#define VENDOR_GFX_GRPH_IMG_ID_B 1
#define VENDOR_GFX_GRPH_IMG_ID_C 2
#define VENDOR_GFX_GRPH_IMG_ID_D 3
typedef struct {
 int command,format;
 struct { int en,id; struct { int property; } data; } property[1];
 struct { int img_id; UINTPTR dram_addr; UINT32 lineoffset,width,height; } images[4];
} VENDOR_GFX_GRPH_TRIGGER_PARAM;
static unsigned gfx_calls;
static inline int vendor_gfx_grph_trigger(int n, void *p) {
 VENDOR_GFX_GRPH_TRIGGER_PARAM *v=p;
 UINT16 *raw=(UINT16*)v->images[0].dram_addr, *k=(UINT16*)v->images[1].dram_addr;
 UINT16 *out=(UINT16*)v->images[2].dram_addr; short *b=(short*)v->images[3].dram_addr;
 (void)n;gfx_calls++;
 for(unsigned i=0;i<v->images[0].width/2*v->images[0].height;i++)
  out[i]=(UINT16)(((long long)k[i]*(raw[i]-b[i]))>>v->property[0].data.property);
 return 0;
}
'''
HARNESS = r'''
#include <assert.h>
#include <sys/stat.h>
#ifdef _WIN32
#define mkdir(p,m) mkdir(p)
#endif
#include "nuc_under_test.c"
static int bpr_on, cfg_nuc, cfg_bpr, loaded_bpr, fault;
static char events[128];
static unsigned event_count;
static void event(char c) {assert(event_count+1<sizeof(events));events[event_count++]=c;events[event_count]=0;}
int app_config_update_int(const char *key,int v) {
 if(strcmp(key,"nuc_correction")==0) {
  event(v?'N':'O');
  if((!v && fault==1) || (v && fault==4)) return -1;
  cfg_nuc=v;
  if(v && fault==5) return -1; /* CFG saved, runtime apply failed. */
  if(v) assert(g_nuc.table_loaded && loaded_bpr && bpr_on);
  nuc_set_enabled(v);return 0;
 }
 assert(strcmp(key,"bpr_correction")==0);event('B');
 if(fault==3)return -1;
 assert(loaded_bpr);cfg_bpr=v;bpr_on=v;return 0;
}
int app_config_update_sensor_temp_points(const float *p,unsigned int n) {(void)p;(void)n;return 0;}
int bpr_load_table(void) {event('L');if(fault==2)return -1;loaded_bpr=1;return 0;}
int bpr_is_enabled(void) {return bpr_on;}
int sensor_temp_read(float *t) {*t=25;return 0;}
static const UINT16 refs[5][4] = {
 {1000,2000,3000,4000}, {2000,3500,5000,5500},
 {3000,4500,7500,7000}, {4000,6500,8500,9000},
 {6000,7500,10500,12000}
};
static int captures, fail_capture=-1;
static int capture(UINTPTR dst,UINT32 size) {
 assert(!nuc_is_enabled() && bpr_on && loaded_bpr && cfg_nuc==0 && cfg_bpr==1);
 event('C');
 if(captures==fail_capture) return -1;
 assert(size==sizeof(refs[0]));
 memcpy((void*)dst,refs[captures/4],size); captures++; return 0;
}
static void input(void) {
 FILE *f=fopen("input.txt","w"); assert(f);
 fputs("\n\n\n\n\n",f); fclose(f);
 assert(freopen("input.txt","r",stdin)); captures=0;event_count=0;events[0]=0;loaded_bpr=0;
}
static void read_table(unsigned p,const char *type,UINT16 *out) {
 char path[128]; nuc_format_table_path(path,sizeof(path),NUC_BLACKBODY_TEMP_DIR,p,type,"");
 assert(nuc_read_file_to_mem(path,out,8)==0);
}
int main(void) {
 UINT16 k[4],b[4],raw[4],expected_k[4][4],expected_b[4][4];
 NUC_IMAGE_LAYOUT layout={4,1,0,0,4,1};
 NUC_TABLE_TRANSACTION_FILE files[NUC_BLACKBODY_TABLE_FILE_COUNT];
 HD_VIDEO_FRAME frame={0},out_frame={0};
 UINT16 output[4];
 unsigned i,j,t;
 assert(mkdir("thermal",0755)==0);
 nuc_bind_runtime((UINTPTR)k,(UINTPTR)b,8,&layout);
 nuc_bind_capture_callback(capture);
 assert(nuc_init_table()!=0 && !nuc_is_enabled());
 nuc_set_enabled(1);assert(!nuc_is_enabled());
 assert(nuc_prepare_table_transaction(files,NUC_SENSOR_TEMP_DIR,0)==10);
 assert(strstr(files[8].final_path,"point5_K.bin"));
 assert(nuc_prepare_table_transaction(files,NUC_BLACKBODY_TEMP_DIR,1)==13);
 assert(strstr(files[12].final_path,"point5_RAW.bin"));
 input(); assert(nuc_five_point_blackbody_temp_calibration()==0); assert(captures==20);
 assert(nuc_validate_all_blackbody_temp_tables()==0);
 assert(nuc_is_enabled() && g_nuc.table_loaded && cfg_nuc==1 && cfg_bpr==1);
 assert(strcmp(events,"OLBCCCCCCCCCCCCCCCCCCCCN")==0);
 nuc_set_enabled(1);assert(nuc_is_enabled());
 for(i=0;i<5;i++) { read_table(i,"RAW",raw); assert(memcmp(raw,refs[i],8)==0); }
 for(i=0;i<4;i++) {
  unsigned low=0,high=0;
  assert(nuc_build_table(refs[i],refs[i+1],expected_k[i],expected_b[i])==0);
  read_table(i,"K",k);read_table(i,"B",b);
  assert(memcmp(k,expected_k[i],8)==0 && memcmp(b,expected_b[i],8)==0);
  for(j=0;j<4;j++) {low+=refs[i][j];high+=refs[i+1][j];}
  low/=4;high/=4;
  for(j=0;j<4;j++) {
   assert(k[j]==256*(high-low)/(refs[i+1][j]-refs[i][j]));
   /* Both blackbody endpoints should map near the frame means (Q8 error). */
   assert(abs((int)(((long long)k[j]*(refs[i][j]-(short)b[j]))>>8)-(int)low)<=2);
   assert(abs((int)(((long long)k[j]*(refs[i+1][j]-(short)b[j]))>>8)-(int)high)<=12);
  }
 }
 assert(expected_k[0][0]!=256);
 /* Failed later acquisition must leave every committed file unchanged. */
 input();fail_capture=8;assert(nuc_five_point_blackbody_temp_calibration()!=0);
 assert(!nuc_is_enabled() && cfg_nuc==0 && bpr_on);
 assert(strchr(events,'N')==NULL);
 for(i=0;i<5;i++) {read_table(i,"RAW",raw);assert(memcmp(raw,refs[i],8)==0);}
 for(i=0;i<4;i++) {read_table(i,"K",k);read_table(i,"B",b);assert(memcmp(k,expected_k[i],8)==0 && memcmp(b,expected_b[i],8)==0);}
 for(i=0;i<13;i++) assert(access(files[i].stage_path,F_OK)!=0);
 assert(nuc_reload_table()==0);
 nuc_set_enabled(1);assert(nuc_is_enabled());
 frame.blk=(UINTPTR)raw;frame.phy_addr[0]=(UINTPTR)raw;frame.loff[0]=8;frame.ph[0]=1;frame.dim.w=4;frame.dim.h=1;
 /* Per-pixel boundaries, equality to upper segment, and both extremes. */
 for(t=0;t<11;t++) {
  unsigned chosen;
  if(t==0 || t==10) {for(j=0;j<4;j++)raw[j]=t==0?0:65535;chosen=t==0?0:3;}
  else {unsigned boundary=(t-1)/3, offset=(t-1)%3; chosen=boundary+(offset!=0);
   for(j=0;j<4;j++)raw[j]=refs[boundary+1][j]+offset-1;
  }
  assert(nuc_apply(&frame,&out_frame,(UINTPTR)k,(UINTPTR)b,(UINTPTR)output,(UINTPTR)output,4,1)==HD_OK);
  assert(memcmp(k,expected_k[chosen],8)==0 && memcmp(b,expected_b[chosen],8)==0);
  for(j=0;j<4;j++)assert(output[j]==(UINT16)(((long long)expected_k[chosen][j]*(raw[j]-(short)expected_b[chosen][j]))>>8));
 }
 for(j=0;j<4;j++)raw[j]= j==0?0:refs[j][j];
 assert(nuc_apply(&frame,&out_frame,(UINTPTR)k,(UINTPTR)b,(UINTPTR)output,(UINTPTR)output,4,1)==HD_OK);
 for(j=0;j<4;j++)assert(k[j]==expected_k[j][j] && b[j]==expected_b[j][j]);
 nuc_release_blackbody_dynamic_sources();
 assert(gfx_calls==12);
 nuc_set_enabled(0);assert(!nuc_is_enabled());
 assert(nuc_apply(&frame,&out_frame,(UINTPTR)k,(UINTPTR)b,(UINTPTR)output,(UINTPTR)output,4,1)!=HD_OK);
 assert(gfx_calls==12);
 /* Simulate restart: only Blackbody files exist, no Sensor Temperature files. */
 nuc_bind_runtime((UINTPTR)k,(UINTPTR)b,8,&layout);
 assert(nuc_init_table()==0 && nuc_is_enabled());
 nuc_set_enabled(0);nuc_set_enabled(1);assert(nuc_is_enabled());
 /* Recalibration completes with BPR and NUC on and CFG saved. */
 fail_capture=-1;input();assert(nuc_five_point_blackbody_temp_calibration()==0);
 assert(nuc_is_enabled() && g_nuc.table_loaded);
 nuc_release_blackbody_dynamic_sources();
 /* Failures before capture must not touch committed tables. */
 for(fault=1;fault<=3;fault++) {
  nuc_set_enabled(0);input();
  assert(nuc_five_point_blackbody_temp_calibration()!=0);
  assert(captures==0 && !nuc_is_enabled());
  assert(strchr(events,'C')==NULL && strchr(events,'N')==NULL);
  for(i=0;i<4;i++){read_table(i,"K",k);assert(memcmp(k,expected_k[i],8)==0);}
 }
 /* A failed final CFG save or runtime enable must leave NUC off. */
 for(fault=4;fault<=5;fault++) {
  input();assert(nuc_five_point_blackbody_temp_calibration()!=0);
  assert(captures==20 && !nuc_is_enabled() && cfg_nuc==0 && bpr_on);
  assert(strcmp(events,"OLBCCCCCCCCCCCCCCCCCCCCNO")==0);
 }
 fault=0;input();assert(nuc_five_point_blackbody_temp_calibration()==0);
 assert(nuc_is_enabled() && cfg_nuc==1 && bpr_on && cfg_bpr==1);
 nuc_release_blackbody_dynamic_sources();
 /* Invalid single-pixel ordering falls back to frame-mean boundaries. */
 memcpy(raw,refs[2],8);raw[0]=refs[1][0];
 assert(nuc_write_mem_to_file("thermal/blackbody_temp/point3_RAW.bin",raw,8)==0);
 assert(nuc_load_blackbody_dynamic_sources()==0);
 assert(g_nuc.blackbody_invalid_pixel_count==1);
 assert(g_nuc.blackbody_thresholds[0]==4000);
 nuc_release_blackbody_dynamic_sources();
 /* Invalid global ordering must reject activation. */
 assert(nuc_write_mem_to_file("thermal/blackbody_temp/point3_RAW.bin",refs[0],8)==0);
 assert(nuc_load_blackbody_dynamic_sources()!=0);
 /* Legacy point K/B cannot replace a missing segment. */
 assert(rename("thermal/blackbody_temp/segment1_K.bin","thermal/blackbody_temp/point1_K.bin")==0);
 assert(nuc_validate_all_blackbody_temp_tables()!=0);
 nuc_set_enabled(1);assert(!nuc_is_enabled());
 assert(nuc_apply(&frame,&out_frame,(UINTPTR)k,(UINTPTR)b,(UINTPTR)output,(UINTPTR)output,4,1)!=HD_OK);
 assert(gfx_calls==12);
 /* Two-point edge cases retain original behavior; dummy pixels are identity. */
 {
  UINT16 low[4]={100,100,100,100},high[4]={100,200,200,200};
  assert(nuc_build_table(low,high,k,b)==0);assert(k[0]==256 && b[0]==0);
  high[0]=99;assert(nuc_build_table(low,high,k,b)!=0);
  g_nuc.active_x=1;g_nuc.active_width=2;
  assert(nuc_build_table(low,high,k,b)==0);
  assert(k[0]==256 && b[0]==0 && k[3]==256 && b[3]==0);
 }
 puts("PASS: capture, adjacent K/B, 13-file transaction, failure preservation, per-pixel segments, boundaries, fallback, legacy rejection, default apply, on/off, startup, recalibration, BPR/NUC sequencing and CFG failures");
 return 0;
}
'''

def main():
    gcc = sys.argv[1] if len(sys.argv)>1 else shutil.which('gcc')
    if not gcc and os.name=='nt':
        gcc = r'C:\msys64\ucrt64\bin\gcc.exe'
    if not gcc:
        raise SystemExit("GCC is required for the host regression")
    env = os.environ.copy()
    env["PATH"] = str(Path(gcc).resolve().parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix='nuc_segments_') as temp:
        work=Path(temp)
        source_path=ROOT/'Correction/NUC/nuc.c'
        source=source_path.read_text(encoding='utf-8')
        source=re.sub(r'#include "([^"]+)"',lambda m: '#include "'+(source_path.parent/m[1]).resolve().as_posix()+'"' if m[1]!='vendor_gfx.h' else m[0],source)
        source=source.replace('/mnt/sd/thermal_op','thermal')
        for name,data in [('hdal.h',HDAL),('vendor_gfx.h',GFX),('nuc_under_test.c',source),('test.c',HARNESS)]:
            (work/name).write_text(data,encoding='utf-8')
        subprocess.run([gcc,'-std=gnu99','-Wall','-Wextra','-Werror','-I',str(work),str(work/'test.c'),'-o',str(work/'test.exe')],env=env,check=True)
        subprocess.run([str(work/'test.exe')],cwd=work,env=env,check=True)

if __name__=='__main__':
    main()
