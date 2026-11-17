from pathlib import Path
import json,hashlib
import cv2,numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

root=Path('D:/01_Project/02_Loewe/01_Report/20261111/display_diag2');out=root/'analysis';out.mkdir(exist_ok=True)
rp=root/'display_diag_0001_videoproc_y8_640x480.raw';jp=root/'display_diag_0001_uvc_mjpeg_640x480.jpg'
b=rp.read_bytes();assert len(b)==307200
raw=np.frombuffer(b,dtype=np.uint8).reshape(480,640)
jpg=cv2.imdecode(np.frombuffer(jp.read_bytes(),np.uint8),cv2.IMREAD_GRAYSCALE);assert jpg.shape==raw.shape
native_raw=raw
native_correlation=float(np.corrcoef(raw.ravel(),jpg.ravel())[0,1])
mirrored_correlation=float(np.corrcoef(raw[:,::-1].ravel(),jpg.ravel())[0,1])
assert mirrored_correlation > native_correlation and mirrored_correlation > .95
raw=np.ascontiguousarray(raw[:,::-1])
regions={'whole':(0,0,640,480),'left':(32,96,256,320),'upper_right':(384,64,544,192),'lower_right':(448,320,608,432)}
report={'scope':'Single uncompressed VideoProc Y8 and independently requested UVC JPEG. Same diagnostic number does not guarantee same frame/timestamp; spatial grid comparisons are independent, pixel residual cannot be attributed entirely to JPEG quantization. No source changes.',
 'files':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [rp,jp]},'images':{}}
report['orientation_alignment']={'raw_horizontal_flip_applied':True,'native_correlation':native_correlation,'aligned_correlation':mirrored_correlation,'note':'The Y8 and JPEG have opposite horizontal orientation. Only diagnostic views/statistics are aligned; source files are unchanged.'}
cv2.imwrite(str(out/'videoproc_y8_native.png'),native_raw)
for name,img in [('videoproc_y8',raw),('uvc_jpeg',jpg)]:
 data=img.astype(float);r={}
 for region,(x0,y0,x1,y1) in regions.items():
  a=data[y0:y1,x0:x1];gx=np.abs(np.diff(a,axis=1)).mean(axis=0);gy=np.abs(np.diff(a,axis=0)).mean(axis=1)
  xx=np.arange(x0+1,x1);yy=np.arange(y0+1,y1);result={}
  for step in [4,8,16,32]:
   px=[float(gx[xx%step==i].mean()) for i in range(step)];py=[float(gy[yy%step==i].mean()) for i in range(step)]
   result[str(step)]={'horizontal_phase':px,'vertical_phase':py,'horizontal_boundary_ratio':float(px[0]/np.mean(px[1:])),'vertical_boundary_ratio':float(py[0]/np.mean(py[1:]))}
  r[region]=result
 report['images'][name]={'min':int(img.min()),'max':int(img.max()),'mean':float(img.mean()),'std':float(img.std()),'regions':r}
 cv2.imwrite(str(out/(name+'.png')),img)
 print(name,'range',img.min(),img.max(),'mean',img.mean())
 for region,bygrid in r.items():print(region,'8x8',bygrid['8']['horizontal_boundary_ratio'],bygrid['8']['vertical_boundary_ratio'])

# Shared contrast transform and nearest-neighbor crop to avoid hiding block edges.
crop=(64,144,192,272);x0,y0,x1,y1=crop
shared_center=float(raw[y0:y1,x0:x1].mean())
panels=[]
for name,img in [('VideoProc Y8 (uncompressed)',raw),('UVC JPEG',jpg)]:
 a=np.clip((img[y0:y1,x0:x1].astype(float)-shared_center)*4+128,0,255).astype(np.uint8)
 a=cv2.resize(a,None,fx=4,fy=4,interpolation=cv2.INTER_NEAREST)
 panel=cv2.copyMakeBorder(cv2.cvtColor(a,cv2.COLOR_GRAY2BGR),36,0,0,0,cv2.BORDER_CONSTANT,value=(0,0,0))
 cv2.putText(panel,name,(10,25),cv2.FONT_HERSHEY_SIMPLEX,.65,(255,255,255),1,cv2.LINE_AA);panels.append(panel)
cv2.imwrite(str(out/'crop_comparison_4x.png'),np.hstack(panels))

fig,axs=plt.subplots(2,2,figsize=(12,8),constrained_layout=True)
for ax,region in zip(axs.flat,regions):
 for label,color in [('videoproc_y8','#188a7e'),('uvc_jpeg','#c47731')]:
  grid=report['images'][label]['regions'][region]['8']
  for axis,style in [('horizontal','-'),('vertical','--')]:
   v=np.array(grid[axis+'_phase']);ax.plot(range(8),v/v.mean(),style+'o',color=color,label=label+' '+axis)
 ax.set(title=region,xlabel='Pixel phase modulo 8 (0 = block boundary)',ylabel='Normalized neighboring-pixel difference');ax.legend(fontsize=7)
fig.savefig(out/'grid8_comparison.png',dpi=140);plt.close(fig)
report['raw_jpeg_mean_abs_difference_not_same_frame_proof']=float(np.abs(raw.astype(float)-jpg.astype(float)).mean())
report['source_hashes_verified']=all(hashlib.sha256(p.read_bytes()).hexdigest()==report['files'][p.name] for p in [rp,jp])
(out/'analysis.json').write_text(json.dumps(report,indent=2),encoding='utf-8')


