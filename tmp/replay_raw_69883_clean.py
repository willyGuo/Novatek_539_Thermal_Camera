"""Separate abnormal captures before measuring ordinary RAW/AGC noise."""
import ast,ctypes as ct,json,os,hashlib
from pathlib import Path
import numpy as np
root=Path('D:/01_Project/02_Loewe/01_Report/20261111/recording_69883_0000');out=root/'analysis'
tree=ast.parse((Path(__file__).parent/'analyze_raw_69883.py').read_text(encoding='utf-8'))
nodes=[n for n in tree.body if (isinstance(n,ast.FunctionDef) and n.name=='metrics') or (isinstance(n,ast.ClassDef) and n.name=='Params')]
exec(compile(ast.fix_missing_locations(ast.Module(body=nodes,type_ignores=[])),'metrics','exec'))
all_frames=np.stack([np.fromfile(p,dtype='<u2').reshape(480,640) for p in sorted(root.glob('*.raw'))])
indices=np.array([i for i in range(100) if i not in [1,72]]);a=all_frames[indices]
def summarize(b):
 r=metrics(b);d=np.diff(b.astype(np.float32),axis=0)[np.diff(indices)==1];dc=d-d.mean(axis=(1,2))[:,None,None]
 r['adjacent_change_rms_median']=float(np.median(np.sqrt(np.mean(d*d,axis=(1,2)))))
 r['global_removed_adjacent_rms_median']=float(np.median(np.sqrt(np.mean(dc*dc,axis=(1,2)))))
 return r
report=dict(scope='Host replay on current C. This analysis separates clear frame72 corruption and separate low-mean frame1. These two captures remain in original files and full-sequence report. Settings are hypothetical until user confirms. No proof of deployed board AGC settings or BNR state.',excluded_indices=[1,72],kept_frames=len(a),anomalies={'frame1':{'mean':float(all_frames[1].mean()),'description':'whole-frame level about 40 DN lower than adjacent captures'},'frame72':{'value40_count':int((all_frames[72]==40).sum()),'value40_percent':float((all_frames[72]==40).mean()*100),'first_value40_pixel_offset':2560,'last_value40_pixel_offset':307039,'byte_span_start':5120,'byte_span_end_exclusive':614080}},raw=summarize(a),replay={})
os.add_dll_directory('C:/msys64/ucrt64/bin');lib=ct.CDLL(str(out/'agc_replay.dll'));lib.allocate.restype=ct.c_void_p;lib.release.argtypes=[ct.c_void_p];lib.lut.argtypes=[ct.c_void_p,ct.c_void_p];lib.agc_weighted_default_params.argtypes=[ct.POINTER(Params)];lib.agc_weighted_state_reset.argtypes=[ct.c_void_p];lib.agc_weighted_apply_u14.argtypes=[ct.c_void_p,ct.c_size_t,ct.c_void_p,ct.c_size_t,ct.c_uint32,ct.c_uint32,ct.c_void_p,ct.POINTER(Params)]
ws=lib.allocate();buf=np.empty_like(a[0]);lut=np.empty(16384,np.uint16)
for bright,gain,smooth in [(1,20,0),(1,10,0),(1,20,90),(3,20,0)]:
 p=Params();lib.agc_weighted_default_params(ct.byref(p));p.clip=0;p.redistribute=0;p.bright=bright;p.percentile=800;p.gain=gain;p.smooth=smooth;p.background=0;p.large=0
 lib.agc_weighted_state_reset(ws);seq=[];first=None
 for frame in a:
  assert lib.agc_weighted_apply_u14(frame.ctypes.data,1280,buf.ctypes.data,1280,640,480,ws,ct.byref(p))==0
  seq.append(buf.copy())
  if first is None:lib.lut(ws,lut.ctypes.data);first=lut.copy()
 seq=np.stack(seq).astype(np.float32)*(255/16383);fixed=first[a].astype(np.float32)*(255/16383)
 key=f'bright{bright}_gain{gain/10:g}_smooth{smooth}'
 report['replay'][key]={'dynamic':summarize(seq),'fixed_first':summarize(fixed)}
 print(key, 'dynamic',json.dumps(report['replay'][key]['dynamic']),'fixed',json.dumps(report['replay'][key]['fixed_first']),flush=True)
lib.release(ws)
(out/'clean_sequence_replay.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('RAW_CLEAN',json.dumps(report['raw']),flush=True)
