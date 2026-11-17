from pathlib import Path
import json,hashlib
import cv2,numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

src=Path('C:/Users/pega_user/Pictures/Camera Roll/WIN_20261111_11_36_37_Pro.mp4')
out=Path('D:/01_Project/02_Loewe/01_Report/20261111/video_113637_analysis');out.mkdir(exist_ok=True)
c=cv2.VideoCapture(str(src));fps=c.get(cv2.CAP_PROP_FPS);code=int(c.get(cv2.CAP_PROP_FOURCC));fourcc=''.join(chr(code>>(8*i)&255) for i in range(4));frames=[];times=[]
while True:
 ok,f=c.read()
 if not ok:break
 frames.append(cv2.cvtColor(f,cv2.COLOR_BGR2GRAY));times.append(c.get(cv2.CAP_PROP_POS_MSEC)/1000)
c.release();a=np.array(frames,dtype=np.float32);n,h,w=a.shape;delta=np.diff(a,axis=0)
regions={'whole':(0,0,w,h),'left_flat':(32,96,256,320),'upper_right':(384,64,544,192),'lower_right':(448,320,608,432)}
report={'source':str(src),'sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'frames':n,'fps':fps,'duration_s':n/fps,'decoded_fourcc':fourcc,'regions':{},'scope':'MP4 decoded 8-bit frames only, possible camera MJPEG and subsequent recording compression. Block statistics locate periodic structure; do not uniquely identify which encoder/ISP stage created it.'}
for name,(x0,y0,x1,y1) in regions.items():
 v=a[:,y0:y1,x0:x1];d=np.diff(v,axis=0)
 gx=np.abs(np.diff(v,axis=2)).mean(axis=(0,1));gy=np.abs(np.diff(v,axis=1)).mean(axis=(0,2))
 tx=np.abs(np.diff(d,axis=2)).mean(axis=(0,1));ty=np.abs(np.diff(d,axis=1)).mean(axis=(0,2))
 res={}
 for step in [4,8,16,32]:
  xx=np.arange(x0+1,x1);yy=np.arange(y0+1,y1)
  profiles={'horizontal_spatial':[float(gx[xx%step==p].mean()) for p in range(step)],'vertical_spatial':[float(gy[yy%step==p].mean()) for p in range(step)],'horizontal_temporal':[float(tx[xx%step==p].mean()) for p in range(step)],'vertical_temporal':[float(ty[yy%step==p].mean()) for p in range(step)]}
  ratios={k:float(values[0]/np.mean(values[1:])) for k,values in profiles.items()}
  res[str(step)]={'boundary_to_interior_ratio':ratios,'phase_profiles':profiles}
 # Adjacent-pixel delta correlation: compare crossing 8-pixel grid to inside.
 def corr(x,y):
  x=x.ravel().astype(np.float64);y=y.ravel().astype(np.float64);x-=x.mean();y-=y.mean()
  return float(np.dot(x,y)/np.sqrt(np.dot(x,x)*np.dot(y,y)))
 xx=np.arange(x0+1,x1);yy=np.arange(y0+1,y1)
 coh={'horizontal_inside':corr(d[:,:,:-1][:,:,xx%8!=0],d[:,:,1:][:,:,xx%8!=0]),'horizontal_across8':corr(d[:,:,:-1][:,:,xx%8==0],d[:,:,1:][:,:,xx%8==0]),'vertical_inside':corr(d[:,:-1,:][:,yy%8!=0,:],d[:,1:,:][:,yy%8!=0,:]),'vertical_across8':corr(d[:,:-1,:][:,yy%8==0,:],d[:,1:,:][:,yy%8==0,:])}
 report['regions'][name]={'bounds':[x0,y0,x1,y1],'grid':res,'delta_correlation':coh}
print(json.dumps({k:v for k,v in report.items() if k!='regions'},indent=2))
for name,v in report['regions'].items():print(name,'grid8 ratios',v['grid']['8']['boundary_to_interior_ratio'],'delta correlation',v['delta_correlation'])

fig,axs=plt.subplots(2,2,figsize=(12,8),constrained_layout=True)
for ax,key,title in zip(axs.flat,['horizontal_spatial','vertical_spatial','horizontal_temporal','vertical_temporal'],['Spatial horizontal differences','Spatial vertical differences','Temporal-change horizontal differences','Temporal-change vertical differences']):
 for name,v in report['regions'].items():
  values=np.array(v['grid']['8']['phase_profiles'][key]);ax.plot(range(8),values/values.mean(),'o-',label=name)
 ax.set(title=title,xlabel='Pixel-coordinate phase modulo 8 (0 = block boundary)',ylabel='Normalized difference');ax.legend(fontsize=8)
fig.savefig(out/'grid8_phase_analysis.png',dpi=140);plt.close(fig)

# Fixed-intensity nearest-neighbor crops; no smoothing or per-frame auto stretch.
x0,y0,x1,y1=regions['left_flat'];crop=(64,144,192,272)
for i in [0,10,20,30]:
 x0,y0,x1,y1=crop;im=a[i,y0:y1,x0:x1].astype(np.uint8)
 zoom=cv2.resize(im,None,fx=4,fy=4,interpolation=cv2.INTER_NEAREST)
 cv2.imwrite(str(out/f'crop_{i:03}_4x.png'),zoom)
 contrast=np.clip((im.astype(float)-im.mean())*4+128,0,255).astype(np.uint8)
 cv2.imwrite(str(out/f'crop_{i:03}_4x_contrast.png'),cv2.resize(contrast,None,fx=4,fy=4,interpolation=cv2.INTER_NEAREST))

diff=np.mean(np.abs(delta),axis=0)
fig,ax=plt.subplots(figsize=(10,7));im=ax.imshow(diff,cmap='magma',vmin=0,vmax=np.percentile(diff,99));fig.colorbar(im,ax=ax,label='Mean absolute frame-to-frame change (Gray)');ax.set(title='Temporal-change spatial pattern',xlabel='x',ylabel='y');fig.tight_layout();fig.savefig(out/'temporal_change_map.png',dpi=140);plt.close(fig)
(out/'analysis.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
