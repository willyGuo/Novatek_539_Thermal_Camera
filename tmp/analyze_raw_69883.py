from pathlib import Path
import csv,json,hashlib,os,subprocess,ctypes as ct
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

root=Path('D:/01_Project/02_Loewe/01_Report/20261111/recording_69883_0000');out=root/'analysis';out.mkdir(exist_ok=True)
repo=Path(__file__).resolve().parents[1]
items=[];arr=[]
for p in sorted(root.glob('*.raw')):
 b=p.read_bytes();assert len(b)==614400
 a=np.frombuffer(b,dtype='<u2').reshape(480,640);u=np.unique(a);d=np.diff(u.astype(np.int64))
 items.append(dict(file=p.name,sha256=hashlib.sha256(b).hexdigest(),min=int(a.min()),max=int(a.max()),unique_levels=len(u),minimum_step=int(d.min()) if len(d) else 0,mod4=np.bincount((a%4).ravel(),minlength=4).tolist(),above_raw14=int((a>16383).sum())))
 arr.append(a)
assert len(arr)==100
a=np.stack(arr);means=a.mean(axis=(1,2));all_mod=np.bincount((a%4).ravel(),minlength=4)
with (root/'summary.csv').open(encoding='utf-8-sig',newline='') as f:meta=list(csv.DictReader(f))
elapsed=np.array([float(r['elapsed_ms']) for r in meta])/1000
capture=np.array([float(r['capture_unix_ms']) for r in meta])/1000
def metrics(b):
 b=np.asarray(b,dtype=np.float32);m=b.mean(axis=(1,2));center=b-m[:,None,None]
 sd=np.std(b,axis=0,ddof=1);sd_center=np.std(center,axis=0,ddof=1)
 diff=b[1:]-b[:-1]; rms=np.sqrt(np.mean(diff*diff,axis=(1,2)))
 c=diff-np.diff(m)[:,None,None];rmsc=np.sqrt(np.mean(c*c,axis=(1,2)))
 return dict(frame_mean_min=float(m.min()),frame_mean_max=float(m.max()),frame_mean_peak_to_peak=float(np.ptp(m)),temporal_sd_median=float(np.median(sd)),temporal_sd_p95=float(np.percentile(sd,95)),global_removed_temporal_sd_median=float(np.median(sd_center)),adjacent_change_rms_median=float(np.median(rms)),adjacent_change_rms_p5_p95=np.percentile(rms,[5,95]).tolist(),global_removed_adjacent_rms_median=float(np.median(rmsc)),adjacent_abs_pixel_change_p50_p95_p99=np.percentile(np.abs(diff),[50,95,99]).tolist())
report=dict(path=str(root),frames=len(a),duration_elapsed_s=float(elapsed[-1]-elapsed[0]),capture_timestamp_span_s=float(capture[-1]-capture[0]),capture_delta_median_s=float(np.median(np.diff(capture))),input_min=int(a.min()),input_max=int(a.max()),above_raw14=int((a>16383).sum()),mod4_percent=(all_mod/a.size*100).tolist(),all_divisible_by4=bool(np.all(a%4==0)),raw_metrics=metrics(a),frame_audits=items)
print('RAW',json.dumps({k:v for k,v in report.items() if k!='frame_audits'},indent=2),flush=True)
class Params(ct.Structure):
 _fields_=[(n,ct.c_double if n=='bright' else ct.c_uint32) for n in ['clip','redistribute','bright','percentile','gain','smooth','background','low','high','target','span','maximum','large']]
wrapper=out/'agc_replay_wrapper.c'
wrapper.write_text('#include <stdlib.h>\n#include <string.h>\n#include "agc_weighted.h"\n#include "agc_mode.h"\nAGC_MODE agc_mode_get(void){return AGC_MODE_WEIGHTED_CPU;}\nvoid *allocate(void){return calloc(1,sizeof(AGC_WEIGHTED_WORKSPACE));}\nvoid release(void *p){free(p);}\nvoid lut(AGC_WEIGHTED_WORKSPACE *p,void *out){memcpy(out,p->lut,sizeof(p->lut));}\n',encoding='utf-8')
cc=Path('C:/msys64/ucrt64/bin/gcc.exe');env=os.environ.copy();env['PATH']=str(cc.parent)+os.pathsep+env.get('PATH','')
libpath=out/'agc_replay.dll'
subprocess.run([str(cc),'-shared','-fPIC','-std=c99','-O2','-Wall','-Wextra','-Werror','-I',str(repo/'Enhancement/AGC'),str(wrapper),str(repo/'Enhancement/AGC/agc_weighted.c'),'-pthread','-lm','-o',str(libpath)],env=env,check=True)
dll_dir=os.add_dll_directory(str(cc.parent));lib=ct.CDLL(str(libpath));lib.allocate.restype=ct.c_void_p;lib.release.argtypes=[ct.c_void_p];lib.lut.argtypes=[ct.c_void_p,ct.c_void_p]
lib.agc_weighted_default_params.argtypes=[ct.POINTER(Params)];lib.agc_weighted_state_reset.argtypes=[ct.c_void_p]
lib.agc_weighted_apply_u14.argtypes=[ct.c_void_p,ct.c_size_t,ct.c_void_p,ct.c_size_t,ct.c_uint32,ct.c_uint32,ct.c_void_p,ct.POINTER(Params)]
ws=lib.allocate();assert ws;tmp=np.empty_like(a[0]);table=np.empty(16384,np.uint16)
cases=[('bright1_gain2_smooth0',1,20,0),('bright1_gain2_smooth90',1,20,90),('bright3_gain2_smooth0',3,20,0),('bright1_gainOFF_smooth0',1,0,0)]
report['replay_scope']='Current production C on host; hypothetical parameters pending user confirmation. Clip0/Redistribution0/P80/Background0/LargeScale0. Equivalent Gray = RAW14*255/16383; before hardware ISP/JPEG. Adjacent RAWs ~100ms apart, unlike 30fps video.'
report['source_sha256']=hashlib.sha256((repo/'Enhancement/AGC/agc_weighted.c').read_bytes()).hexdigest()
report['replay']={};plot_means={};plot_rms={}
for name,bright,gain,smooth in cases:
 p=Params();lib.agc_weighted_default_params(ct.byref(p));p.clip=0;p.redistribute=0;p.bright=bright;p.percentile=800;p.gain=gain;p.smooth=smooth;p.background=0;p.large=0
 lib.agc_weighted_state_reset(ws);seq=[];tables=[]
 for frame in a:
  assert lib.agc_weighted_apply_u14(frame.ctypes.data,1280,tmp.ctypes.data,1280,640,480,ws,ct.byref(p))==0
  seq.append(tmp.copy());lib.lut(ws,table.ctypes.data);tables.append(table.copy())
 seq=np.stack(seq);tables=np.stack(tables);gray=seq.astype(np.float32)*(255/16383)
 report['replay'][name]=metrics(gray);plot_means[name]=gray.mean(axis=(1,2));plot_rms[name]=np.sqrt(np.mean(np.diff(gray,axis=0)**2,axis=(1,2)))
 if smooth==0:
  for idx in [0,50]:
   fixed=tables[idx][a].astype(np.float32)*(255/16383);key=f'{name}_fixed_frame{idx}'
   report['replay'][key]=metrics(fixed)
   if bright==1 and gain==20:
    plot_means[key]=fixed.mean(axis=(1,2));plot_rms[key]=np.sqrt(np.mean(np.diff(fixed,axis=0)**2,axis=(1,2)))
  residual=(seq.astype(np.float32)-tables[0][a].astype(np.float32))*(255/16383)
  report['replay'][name]['dynamic_minus_fixed_first_pixel_rms']=float(np.sqrt(np.mean(residual**2)))
 print(name,json.dumps(report['replay'][name]),flush=True)
lib.release(ws)
(out/'analysis.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
fig,axs=plt.subplots(3,1,figsize=(13,11),constrained_layout=True)
axs[0].plot(elapsed,means);axs[0].set(title='Corrected RAW: mean DN per captured frame',ylabel='RAW DN')
for k,v in plot_means.items():axs[1].plot(elapsed,v-v.mean(),label=k)
axs[1].set(title='Host AGC replay: change in mean brightness',ylabel='Equivalent 8-bit Gray');axs[1].legend(fontsize=8,ncol=2)
for k,v in plot_rms.items():axs[2].plot(elapsed[1:],v,label=k)
axs[2].set(title='Host replay: adjacent-frame pixel-change RMS (~100ms spacing)',ylabel='Equivalent 8-bit Gray',xlabel='Recording elapsed time (s)');axs[2].legend(fontsize=8,ncol=2)
fig.savefig(out/'agc_replay_comparison.png',dpi=130);plt.close(fig)
print('SAVED',out/'analysis.json',flush=True)
