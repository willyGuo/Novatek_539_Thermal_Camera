from pathlib import Path
import json,hashlib
import cv2,numpy as np
from scipy import signal
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
srcdir=Path('D:/01_Project/02_Loewe/08_Tool/Thermal_Image_Toolkit/Thermal_RAW_Results/uvc_viewer_recording/20261111_100444_007')
src=srcdir/'uvc_viewer.avi';out=Path('D:/01_Project/02_Loewe/01_Report/20261111/video_100444_analysis');out.mkdir(exist_ok=True)
cap=cv2.VideoCapture(str(src));fps=cap.get(cv2.CAP_PROP_FPS)
frames=[];pts=[];means=[];spatial=[];rms=[];mad=[];patch=[];dups=[];prev=None
idx=0
while True:
    ok,f=cap.read()
    if not ok:break
    a=cv2.cvtColor(f,cv2.COLOR_BGR2GRAY).astype(np.float32)
    if idx==0:cv2.imwrite(str(out/'first_frame.png'),a.astype(np.uint8))
    pts.append(cap.get(cv2.CAP_PROP_POS_MSEC)/1000)
    means.append(float(a.mean()));spatial.append(float(a.std()))
    patch.append(a.reshape(12,40,16,40).mean(axis=(1,3)).ravel())
    frames.append(a[::4,::4].copy())
    if prev is not None:
        d=a-prev;rms.append(float(np.sqrt(np.mean(d*d))));mad.append(float(np.abs(d).mean()))
        if np.all(d==0):dups.append(idx)
    prev=a;idx+=1
cap.release()
a=np.array(frames);t=np.array(pts);m=np.array(means);sp=np.array(spatial);patch=np.array(patch);rms=np.array(rms)
local=a-m[:,None,None];z=signal.detrend(local,axis=0,type='linear')
def acf(v,maxlag):
    v=np.asarray(v,dtype=np.float64);v=v-v.mean(axis=0)
    ans=[]
    for k in range(1,maxlag+1):
        x=v[:-k];y=v[k:];den=np.sqrt((x*x).sum()*(y*y).sum())
        ans.append(float((x*y).sum()/den) if den else 0)
    return np.array(ans)
lags=acf(z,180)
mf,mp=signal.periodogram(m,fs=fps,detrend='linear');sf,spp=signal.periodogram(sp,fs=fps,detrend='linear')
pf,pp=signal.welch(z,fs=fps,axis=0,nperseg=128);pp=pp.mean(axis=(1,2))
rf,rp=signal.periodogram(rms,fs=fps,detrend='linear')
def peaks(f,p):
    ix=signal.find_peaks(p)[0];ix=ix[f[ix]>=.1];ix=sorted(ix,key=lambda i:p[i],reverse=True)[:6]
    return [dict(hz=float(f[i]),period_s=float(1/f[i]),power_fraction=float(p[i]/p.sum())) for i in ix]
dup_intervals=np.diff(dups)
block_correlations=[]
for start in [0,120,240,360]:
    sub=z[start:start+117]
    block_correlations.append(dict(start_s=start/fps,lag_correlations=acf(sub,60).tolist()))
report=dict(source=str(src),sha256=hashlib.sha256(src.read_bytes()).hexdigest(),frames=len(a),fps=fps,duration_seconds=len(a)/fps,mean_range=[float(m.min()),float(m.max())],mean_peak_to_peak=float(np.ptp(m)),max_adjacent_mean_change=float(np.abs(np.diff(m)).max()),spatial_sd_range=[float(sp.min()),float(sp.max())],sampled_pixel_temporal_sd_median=float(np.median(np.std(a,axis=0,ddof=1))),global_removed_sampled_pixel_temporal_sd_median=float(np.median(np.std(local,axis=0,ddof=1))),adjacent_rms_range=[float(rms.min()),float(rms.max())],nonduplicate_adjacent_rms_median=float(np.median(rms[rms>0])),global_change_energy_fraction=float(np.sum(np.diff(m)**2)/np.sum(rms**2)),exact_duplicate_count=len(dups),exact_duplicate_frames=dups,duplicate_intervals_frames=dup_intervals.tolist(),mean_spectrum_peaks=peaks(mf,mp),spatial_sd_spectrum_peaks=peaks(sf,spp),local_pixel_welch_peaks=peaks(pf,pp),adjacent_rms_spectrum_peaks=peaks(rf,rp),local_acf=lags.tolist(),segment_local_acf=block_correlations,recording_metadata=json.loads((srcdir/'recording.json').read_text(encoding='utf-8')),per_frame=[dict(frame=i,time_s=float(t[i]),mean=float(m[i]),spatial_sd=float(sp[i]),adjacent_change_rms=None if i==0 else float(rms[i-1])) for i in range(len(a))],scope='Decoded MJPG AVI; global/adjacent metrics use all pixels, temporal pixel metrics use every fourth row and column. Recording includes timestamp resampling and duplicate frames. No RAW-stage attribution.')
(out/'analysis.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
fig,axs=plt.subplots(4,1,figsize=(13,13),constrained_layout=True)
axs[0].plot(t,m);axs[0].set(title='Whole-frame mean brightness',ylabel='8-bit Gray')
axs[1].plot(t[1:],rms);axs[1].scatter(t[dups],np.zeros(len(dups)),s=9,c='r',label='Exact repeated frame');axs[1].legend();axs[1].set(title='Adjacent-frame pixel change RMS',ylabel='Gray')
axs[2].plot(t,sp);axs[2].set(title='Spatial standard deviation per frame',xlabel='Time (s)',ylabel='Gray')
axs[3].plot(np.arange(1,181)/fps,lags,label='Full clip')
for b in block_correlations:axs[3].plot(np.arange(1,61)/fps,b['lag_correlations'],alpha=.6,label=f"Segment {b['start_s']:.0f}s")
axs[3].axhline(0,c='gray',lw=1);axs[3].set(title='Local temporal correlation (global brightness removed)',xlabel='Lag (s)',ylabel='Correlation');axs[3].legend(ncol=3)
fig.savefig(out/'temporal_analysis.png',dpi=140);plt.close(fig)
fig,axs=plt.subplots(3,1,figsize=(12,10),constrained_layout=True)
axs[0].plot(mf,mp);axs[0].set(title='Whole-frame mean brightness spectrum',ylabel='Power')
axs[1].plot(pf,pp);axs[1].set(title='Average local-pixel temporal spectrum (Welch)',ylabel='Power')
axs[2].plot(rf,rp);axs[2].set(title='Adjacent-change magnitude spectrum (includes recorder repeats)',ylabel='Power',xlabel='Frequency (Hz)')
for ax in axs:ax.set_xlim(0,15)
fig.savefig(out/'frequency_analysis.png',dpi=140);plt.close(fig)
print(json.dumps({k:v for k,v in report.items() if k not in ['per_frame','local_acf','segment_local_acf','recording_metadata','scope']},indent=2))
print('POSITIVE LOCAL ACF PEAKS',[(i+1,round(float(lags[i]),4)) for i in signal.find_peaks(lags)[0] if lags[i]>.02])
