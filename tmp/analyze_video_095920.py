from pathlib import Path
import hashlib,json
import cv2,numpy as np
from scipy import signal
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

src=Path('C:/Users/pega_user/Pictures/Camera Roll/WIN_20261111_09_59_20_Pro.mp4')
out=Path('D:/01_Project/02_Loewe/01_Report/20261111/video_095920_analysis')
out.mkdir(exist_ok=True)
c=cv2.VideoCapture(str(src)); fps=c.get(cv2.CAP_PROP_FPS);frames=[];pts=[]
while True:
    ok,f=c.read()
    if not ok:break
    frames.append(cv2.cvtColor(f,cv2.COLOR_BGR2GRAY));pts.append(c.get(cv2.CAP_PROP_POS_MSEC)/1000)
c.release()
a=np.array(frames,dtype=np.float32);t=np.array(pts);n,h,w=a.shape
means=a.mean(axis=(1,2));delta=np.diff(a,axis=0)
demeaned=a-means[:,None,None];variation=demeaned-demeaned.mean(axis=0)
std=np.std(a,axis=0,ddof=1);localstd=np.std(demeaned,axis=0,ddof=1)
mad=np.abs(delta).mean(axis=(1,2)); rms=np.sqrt((delta**2).mean(axis=(1,2)))
local_delta=np.diff(demeaned,axis=0)
local_rms=np.sqrt((local_delta**2).mean(axis=(1,2)))
patch=a.reshape(n,12,40,16,40).mean(axis=(2,4)).reshape(n,-1)
patchdiff=np.diff(patch,axis=0)
lags=[]
for k in range(1,min(25,n)):
    x=variation[:-k].ravel();y=variation[k:].ravel()
    lags.append(dict(lag=k,mean_dt_s=float(np.mean(t[k:]-t[:-k])),correlation=float(np.dot(x,y)/np.sqrt(np.dot(x,x)*np.dot(y,y)))))
freq=np.fft.rfftfreq(n,1/fps)
spec=np.abs(np.fft.rfft(signal.detrend(a,axis=0,type='linear'),axis=0))**2
power=spec.mean(axis=(1,2)); power[0]=0
top=np.argsort(power)[-5:][::-1]
byframe=[]
for i in range(n):
    row=dict(frame=i,time_s=float(t[i]),mean=float(means[i]),spatial_sd=float(a[i].std()))
    if i:
        row.update(mean_change=float(means[i]-means[i-1]),mean_abs_pixel_change=float(mad[i-1]),pixel_change_rms=float(rms[i-1]),global_removed_change_rms=float(local_rms[i-1]))
    byframe.append(row)
report=dict(source=str(src),sha256=hashlib.sha256(src.read_bytes()).hexdigest(),frames=n,fps_metadata=fps,first_to_last_s=float(t[-1]-t[0]),duration_estimate_s=n/fps,mean_range=[float(means.min()),float(means.max())],mean_peak_to_peak=float(np.ptp(means)),maximum_adjacent_mean_change=float(np.abs(np.diff(means)).max()),temporal_sd_median=float(np.median(std)),global_removed_temporal_sd_median=float(np.median(localstd)),adjacent_pixel_rms_range=[float(rms.min()),float(rms.max())],global_removed_adjacent_rms_range=[float(local_rms.min()),float(local_rms.max())],global_difference_energy_fraction=float(np.sum(np.diff(means)**2)/np.sum((delta**2).mean(axis=(1,2)))),identical_adjacent_frames=int(np.sum(np.all(delta==0,axis=(1,2)))),spectral_peaks=[dict(hz=float(freq[i]),power_fraction=float(power[i]/power.sum())) for i in top],lags=lags,per_frame=byframe,scope='Decoded MP4 pixels in 8-bit grayscale; compression and capture timing included. Not RAW/BNR/AGC stage proof. Clip is only 48 frames, so stable long-term periodicity cannot be established.')
(out/'analysis.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
fig,axs=plt.subplots(4,1,figsize=(12,12),constrained_layout=True)
axs[0].plot(t,means,'o-',ms=3);axs[0].set(title='Whole-frame mean brightness (decoded 8-bit video)',ylabel='Gray')
axs[1].plot(t[1:],rms,label='Pixel change RMS');axs[1].plot(t[1:],local_rms,label='After removing global brightness change');axs[1].legend();axs[1].set(ylabel='Gray',title='Adjacent-frame changes')
for j in [0,15,88,103,176,191]:axs[2].plot(t,patch[:,j]-patch[:,j].mean(),label=f'Patch {j}')
axs[2].set(ylabel='Gray relative to patch mean',title='Selected 40x40 regional brightness');axs[2].legend(ncol=3)
axs[3].plot([x['mean_dt_s'] for x in lags],[x['correlation'] for x in lags],'o-');axs[3].axhline(0,color='gray',lw=1);axs[3].set(title='Temporal correlation after removing per-frame global brightness',xlabel='Lag (seconds)',ylabel='Correlation')
fig.savefig(out/'temporal_analysis.png',dpi=140);plt.close(fig)
fig,ax=plt.subplots(figsize=(9,6));ax.imshow(localstd,cmap='magma',vmin=0,vmax=np.percentile(localstd,99));im=ax.images[0];fig.colorbar(im,ax=ax,label='Temporal SD (Gray), global brightness removed');ax.set(title='Local temporal variation (decoded video)',xlabel='x',ylabel='y');fig.tight_layout();fig.savefig(out/'temporal_sd.png',dpi=140);plt.close(fig)
fig,axs=plt.subplots(2,4,figsize=(12,5),constrained_layout=True)
for ax,i in zip(axs.flat,[0,6,12,18,24,30,36,42]):ax.imshow(a[i],cmap='gray',vmin=105,vmax=145);ax.set_title(f'{i}: {t[i]:.3f}s');ax.axis('off')
fig.suptitle('Fixed display range 105..145 (for visualizing subtle variations)');fig.savefig(out/'frames_fixed_stretch.png',dpi=140);plt.close(fig)
print(json.dumps({k:v for k,v in report.items() if k not in ['per_frame','lags']},indent=2))
print('LAGS',[(r['lag'],round(r['correlation'],3)) for r in lags])
print('MEANS',means.tolist())
print('RMS',rms.tolist())
