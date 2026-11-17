from pathlib import Path
import json,csv,hashlib,ctypes as ct,os,shutil
import cv2,numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

base=Path('D:/01_Project/02_Loewe/01_Report/20261111')
out=base/'offline_temporal_denoise_v1';out.mkdir(exist_ok=True)
configs=[('baseline',None,0),('light_T8_prev50',8,1),('medium_T12_prev75',12,2),('strong_T16_prev87p5',16,3)]
report={'scope':'Offline static-scene experiment only. Independently process Original and Corrected sequences. Full precision RAW14 is stored in little-endian uint16. Fixed AGC LUT from unfiltered Corrected frame50 is shared by all comparison views. Real production C is used for dynamic Weighted AGC replay. Snapshot timing differs from continuous camera streaming. No firmware modifications, target CPU test, or real-motion ghosting test.',
 'algorithm':'Q8 state. If abs(current*256 - previous_filtered_q8) <= T*256, state = round((previous_filtered_q8*(2**k-1)+current*256)/(2**k)); otherwise state=current*256. Output=round(state/256). k=1/2/3 means previous weight 50/75/87.5%. Reset for capture timestamp gap >0.25s.',
 'configs':configs,'datasets':{}}
datasets={};timings={};file_lists={};hashes={}
for stage,folder in [('corrected','recording_70469_0000'),('original','recording_70741_0000')]:
 root=base/folder;paths=sorted(root.glob('*.raw'));assert len(paths)==100
 data=[];entries=[]
 for p in paths:
  b=p.read_bytes();assert len(b)==614400
  data.append(np.frombuffer(b,dtype='<u2').reshape(480,640));entries.append({'file':p.name,'sha256':hashlib.sha256(b).hexdigest()})
 arr=np.stack(data);assert arr.max()<=16383
 with (root/'summary.csv').open(encoding='utf-8-sig',newline='') as f:metadata=list(csv.DictReader(f))
 timings[stage]=np.array([float(x['capture_unix_ms']) for x in metadata])/1000
 datasets[stage]=arr;file_lists[stage]=paths;hashes[stage]=entries

def denoise(arr,t,threshold,k):
 if threshold is None:return arr.copy(),{'accepted_percent':0.,'reset_frame_indices':[]}
 result=np.empty_like(arr);state=arr[0].astype(np.int32)*256;result[0]=arr[0];accepted=0;tested=0;resets=[]
 for i in range(1,len(arr)):
  current=arr[i].astype(np.int32)*256
  if t[i]-t[i-1]>.25:
   state=current;resets.append(i)
  else:
   close=np.abs(current-state)<=threshold*256
   blended=(state*((1<<k)-1)+current+(1<<(k-1)))>>k
   state=np.where(close,blended,current)
   accepted+=int(close.sum());tested+=close.size
  result[i]=((state+128)>>8).astype(np.uint16)
 return result,{'accepted_percent':accepted/tested*100 if tested else 0,'reset_frame_indices':resets}

def stats(arr,start=10,t=None):
 x=arr[start:].astype(np.float32);m=x.mean(axis=(1,2));center=x-m[:,None,None]
 std=np.std(center,axis=0,ddof=1)
 delta=np.diff(x,axis=0)
 if t is not None:delta=delta[np.diff(t[start:])<=.25]
 delta-=delta.mean(axis=(1,2))[:,None,None]
 rms=np.sqrt(np.mean(delta*delta,axis=(1,2)))
 return {'analyzed_frame_start':start,'frames_used':len(x),'global_removed_temporal_sd_median':float(np.median(std)),'global_removed_temporal_sd_p95':float(np.percentile(std,95)),'global_removed_adjacent_rms_median':float(np.median(rms)),'mean_peak_to_peak':float(np.ptp(m))}

# Host arithmetic checks: static data and a large change pass without lag.
test=np.array([100,100,200],dtype=np.uint16).reshape(3,1,1)
for _,thr,k in configs[1:]:
 y,_=denoise(test,np.arange(3)*.033,thr,k);assert y[:,0,0].tolist()==[100,100,200]
 assert denoise(np.full((3,1,1),16383,np.uint16),np.arange(3)*.033,thr,k)[0].max()==16383

class Params(ct.Structure):
 _fields_=[(n,ct.c_double if n=='bright' else ct.c_uint32) for n in ['clip','redistribute','bright','percentile','gain','smooth','background','low','high','target','span','maximum','large']]
dll_dir=os.add_dll_directory('C:/msys64/ucrt64/bin')
lib=ct.CDLL(str(base/'recording_70469_0000/analysis/agc_replay.dll'));lib.allocate.restype=ct.c_void_p;lib.release.argtypes=[ct.c_void_p];lib.lut.argtypes=[ct.c_void_p,ct.c_void_p];lib.agc_weighted_default_params.argtypes=[ct.POINTER(Params)];lib.agc_weighted_state_reset.argtypes=[ct.c_void_p];lib.agc_weighted_apply_u14.argtypes=[ct.c_void_p,ct.c_size_t,ct.c_void_p,ct.c_size_t,ct.c_uint32,ct.c_uint32,ct.c_void_p,ct.POINTER(Params)]
p=Params();lib.agc_weighted_default_params(ct.byref(p));p.clip=0;p.redistribute=0;p.bright=1;p.percentile=800;p.gain=20;p.smooth=70;p.background=0;p.large=0
ws=lib.allocate();assert ws;buf=np.empty((480,640),np.uint16);lut=np.empty(16384,np.uint16)
assert lib.agc_weighted_apply_u14(datasets['corrected'][50].ctypes.data,1280,buf.ctypes.data,1280,640,480,ws,ct.byref(p))==0
lib.lut(ws,lut.ctypes.data);np.save(out/'shared_fixed_lut_u14.npy',lut)
corrected_variants={};fixed_views={}
for stage,arr in datasets.items():
 stage_report={'input':str(base/('recording_70469_0000' if stage=='corrected' else 'recording_70741_0000')),'source_sha256':hashes[stage],'results':{}}
 for name,threshold,k in configs:
  filtered,extra=denoise(arr,timings[stage],threshold,k)
  result={'raw':stats(filtered,t=timings[stage]),**extra}
  if threshold is not None:
   dest=out/stage/name;dest.mkdir(parents=True,exist_ok=True)
   manifest=[]
   for path,frame in zip(file_lists[stage],filtered):
    target=dest/path.name;frame.astype('<u2').tofile(target);assert target.stat().st_size==614400
    manifest.append({'file':target.name,'sha256':hashlib.sha256(target.read_bytes()).hexdigest()})
   (dest/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
  if stage=='corrected':
   fixed=lut[filtered].astype(np.float32)*(255/16383)
   result['fixed_agc_gray']=stats(fixed,t=timings[stage]);fixed_views[name]=np.clip(np.rint(fixed),0,255).astype(np.uint8)
   # Real AGC replay with each variant's own histogram, actual user parameters.
   lib.agc_weighted_state_reset(ws);dynamic=[]
   for frame in filtered:
    assert lib.agc_weighted_apply_u14(frame.ctypes.data,1280,buf.ctypes.data,1280,640,480,ws,ct.byref(p))==0
    dynamic.append(buf.copy())
   dynamic=np.stack(dynamic).astype(np.float32)*(255/16383)
   result['dynamic_agc_gray']=stats(dynamic,t=timings[stage]);corrected_variants[name]=filtered
  stage_report['results'][name]=result
  print(stage,name,json.dumps(result),flush=True)
 baseline_sd=stage_report['results']['baseline']['raw']['global_removed_temporal_sd_median']
 for result in stage_report['results'].values():result['temporal_sd_reduction_percent']=(1-result['raw']['global_removed_temporal_sd_median']/baseline_sd)*100
 report['datasets'][stage]=stage_report
lib.release(ws)

# Side-by-side view: all panels use the exact same LUT and display transform.
videos=out/'preview';videos.mkdir(exist_ok=True)
names=[x[0] for x in configs];labels=['Baseline','Light: T8 / prev50%','Medium: T12 / prev75%','Strong: T16 / prev87.5%']
for enhanced,filename in [(False,'comparison_same_mapping.avi'),(True,'comparison_noise_magnified.avi')]:
 writer=cv2.VideoWriter(str(videos/filename),cv2.VideoWriter_fourcc(*'MJPG'),10.,(1280,1040),True);assert writer.isOpened()
 for i in range(100):
  panels=[]
  for name,label in zip(names,labels):
   gray=fixed_views[name][i]
   if enhanced:gray=np.clip((gray.astype(np.float32)-128)*3+128,0,255).astype(np.uint8)
   panel=cv2.copyMakeBorder(cv2.cvtColor(gray,cv2.COLOR_GRAY2BGR),40,0,0,0,cv2.BORDER_CONSTANT,value=(15,15,15))
   cv2.putText(panel,label,(12,28),cv2.FONT_HERSHEY_SIMPLEX,.65,(255,255,255),1,cv2.LINE_AA);panels.append(panel)
  canvas=np.vstack((np.hstack(panels[:2]),np.hstack(panels[2:])))
  writer.write(canvas)
  if i==50:cv2.imwrite(str(videos/(filename.replace('.avi','_frame50.png'))),canvas)
 writer.release()
 verify=cv2.VideoCapture(str(videos/filename));assert int(verify.get(cv2.CAP_PROP_FRAME_COUNT))==100;verify.release()

fig,axs=plt.subplots(1,2,figsize=(12,5),constrained_layout=True)
for ax,stage in zip(axs,['corrected','original']):
 vals=[report['datasets'][stage]['results'][name]['raw']['global_removed_temporal_sd_median'] for name in names]
 ax.bar(range(4),vals,color=['#7b8790','#4285a8','#008b80','#c47e2c']);ax.set_xticks(range(4),['Baseline','Light','Medium','Strong']);ax.set(title=f'{stage}: temporal noise (global drift removed)',ylabel='Temporal SD median (RAW DN)',ylim=(0,4.2))
 for i,v in enumerate(vals):ax.text(i,v+.06,f'{v:.2f}',ha='center')
fig.savefig(out/'noise_comparison.png',dpi=150);plt.close(fig)

for stage,paths in file_lists.items():
 for path,item in zip(paths,hashes[stage]):assert hashlib.sha256(path.read_bytes()).hexdigest()==item['sha256']
report['originals_verified_unchanged']=True
report['preview_note']='Preview is a 10fps slideshow of saved samples, NOT recovered camera timing. First video uses fixed mapping unchanged. Second applies common 3x contrast around Gray128 solely to reveal noise; it is labeled magnified and not actual camera output. AVI adds JPEG compression; RAW files retain numeric results.'
(out/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
readme='''離線時間降噪測試

資料：Corrected recording_70469_0000 與 Original recording_70741_0000，各100張，獨立處理。
原始檔未修改，來源與輸出 SHA256 記錄在 results.json / 各目錄 manifest.json。

方法：目前值與上一張濾波結果的差 <= 門檻才平均，否則直接使用目前值。
輕：8 DN / 前一張權重50%；中：12 DN / 75%；強：16 DN / 87.5%。
內部Q8累加，避免整數平均卡住；輸出仍是 RAW14 數值、RAW16 little-endian 儲存。
捕獲時間間隔超過0.25秒時重設歷史。

results.json：統計使用第10～99張，先排除每張平均亮度的漂移；相鄰差值不跨越超過0.25秒的捕獲空隙。
Corrected 有同一張固定AGC映射表的公平比較，另有目前C實作按各自直方圖重算的AGC重播。
AGC參數：Clip0、Redistribution0、BrightWeight1、P80、GainLimit2、Smooth70、Background0、LargeScale0。

preview/comparison_same_mapping.avi：四種結果共用相同映射，沒有額外拉對比。
preview/comparison_noise_magnified.avi：同樣四種結果統一以128為中心放大對比3倍，只用來看細微雜訊。
影片以10fps展示已保存影格，沒有重建原始捕獲時間；MJPEG影片有壓縮，數值以RAW為準。

限制：本次是均溫靜態場景；不能證明移動細節不損失或無拖影。
低於門檻的真實小幅變化仍可能被平均掉。強檔並非正式推薦。
沒有修改韌體或Toolkit，沒有板端CPU/FPS/移動拖影驗證。
'''
(out/'README.txt').write_text(readme,encoding='utf-8')
shutil.copyfile(__file__,out/'offline_temporal_trial.py')
print('COMPLETE',out,flush=True)
