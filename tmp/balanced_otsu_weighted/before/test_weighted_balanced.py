"""Independent host checks for simple Otsu balanced mapping; no board proof."""
import argparse
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import numpy as np
from test_weighted_raw14 import Params, ROOT, N, MAX


def reference(a, p, previous=None):
    a=np.minimum(a,MAX).astype(np.int64)
    values,counts=np.unique(a,return_counts=True)
    left=np.cumsum(counts)[:-1].astype(float);right=a.size-left
    split=False;threshold=0;separation=0.
    if len(values)>1:
        sums=np.cumsum(values.astype(float)*counts)
        differences=(sums[-1]-sums[:-1])/right-sums[:-1]/left
        scores=left*right*differences**2
        index=int(np.argmax(scores));threshold=int(values[index])
        separation=float(differences[index]);split=separation>=70
    center=float(np.median(a[a<=threshold] if split else a))
    gain=min(1.2,p.gain/10) if p.gain else 1.2
    x=np.arange(N);gray=96+gain*(x-center)
    gray=np.minimum(gray,160 if split else 245)
    if split:
        boundary=min(160,96+gain*(threshold-center))
        hot=x>threshold
        gray[hot]=boundary+(245-boundary)*np.minimum(1,(x[hot]-threshold)/(int(values[-1])-threshold))
    raw=np.floor(np.clip(gray,16,245)*MAX/255+.5).astype(np.int64)
    q8=raw*256
    if previous is not None and p.smooth:
        q8=(q8*(100-p.smooth)+previous*p.smooth+50)//100
    history=q8.copy()
    if p.gain:
        cap=round(p.gain*MAX*256/2550)
        q8=q8[0]+np.r_[0,np.cumsum(np.minimum(np.diff(q8),cap))]
    return ((q8+128)//256).astype(np.uint16),history,dict(
        otsu_threshold_dn=threshold if len(values)>1 else None,
        class_mean_separation_dn=separation,hot_segment=bool(split),background_center_dn=center)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--before-dir',type=Path)
    parser.add_argument('--raw-dir',type=Path)
    parser.add_argument('--output-dir',type=Path)
    parser.add_argument('--toolkit',type=Path,help='Accepted for old scripts; R5 is independent of Toolkit v2')
    args=parser.parse_args()
    cc=shutil.which('gcc');assert cc
    env=os.environ.copy();env['PATH']=str(Path(cc).parent)+os.pathsep+env.get('PATH','')
    dll_dir=os.add_dll_directory(str(Path(cc).parent)) if os.name=='nt' else None
    report=dict(scope='Host C/reference only; no SDK link or board/FPS proof',profile='Otsu R5',
        sources={n:hashlib.sha256((ROOT/'Enhancement/AGC'/n).read_bytes()).hexdigest() for n in ['agc_weighted.c','agc_weighted.h']})
    loaded=[]
    with tempfile.TemporaryDirectory(prefix='balanced_otsu_') as directory:
        work=Path(directory)
        wrapper=work/'wrapper.c'
        wrapper.write_text("#include <stdlib.h>\n#include <string.h>\n#include \"agc_weighted.h\"\n#include \"agc_mode.h\"\n"
            "AGC_MODE agc_mode_get(void){return AGC_MODE_WEIGHTED_CPU;}\n"
            "void *allocate(void){return calloc(1,sizeof(AGC_WEIGHTED_WORKSPACE));}\n"
            "void release(void *p){free(p);}\nsize_t size(void){return sizeof(AGC_WEIGHTED_WORKSPACE);}\n"
            "void lut(AGC_WEIGHTED_WORKSPACE *p,void *out){memcpy(out,p->lut,sizeof(p->lut));}\n",encoding='utf-8')
        def library(source,name):
            output=work/(name+('.dll' if os.name=='nt' else '.so'))
            subprocess.run([cc,'-shared','-fPIC','-std=c99','-O2','-Wall','-Wextra','-Werror',
                '-I',str(source),'-I',str(ROOT/'Enhancement/AGC'),str(wrapper),str(source/'agc_weighted.c'),
                '-pthread','-lm','-o',str(output)],check=True,env=env)
            api=ct.CDLL(str(output));loaded.append(api)
            api.allocate.restype=ct.c_void_p;api.release.argtypes=[ct.c_void_p]
            api.size.restype=ct.c_size_t
            api.lut.argtypes=[ct.c_void_p,ct.c_void_p]
            api.agc_weighted_state_reset.argtypes=[ct.c_void_p]
            api.agc_weighted_default_params.argtypes=[ct.POINTER(Params)]
            api.agc_weighted_apply_u14.argtypes=[ct.c_void_p,ct.c_size_t,ct.c_void_p,ct.c_size_t,
                ct.c_uint32,ct.c_uint32,ct.c_void_p,ct.POINTER(Params)]
            return api
        api=library(ROOT/'Enhancement/AGC','current');ws=api.allocate()
        old=library(args.before_dir.resolve(),'before') if args.before_dir else None
        old_ws=old.allocate() if old else None
        def params():
            p=Params();api.agc_weighted_default_params(ct.byref(p));p.balanced=1;p.smooth=0;return p
        def apply(a,p,reset=True,engine=api,state=ws,inplace=False):
            a=np.array(a,dtype=np.uint16,order='C',copy=True);saved=a.copy()
            out=a if inplace else np.empty_like(a)
            if reset:engine.agc_weighted_state_reset(state)
            assert engine.agc_weighted_apply_u14(a.ctypes.data,a.strides[0],out.ctypes.data,out.strides[0],
                a.shape[1],a.shape[0],state,ct.byref(p))==0
            table=np.empty(N,np.uint16);engine.lut(state,table.ctypes.data)
            assert np.diff(table.astype(int)).min()>=0
            np.testing.assert_array_equal(out,table[np.minimum(saved,MAX)])
            if not inplace:np.testing.assert_array_equal(a,saved)
            return out,table
        rng=np.random.default_rng(171117)
        cases=[np.full((40,50),dn,np.uint16) for dn in [0,4500,MAX]]
        cases += [rng.integers(4490,4511,(40,50),dtype=np.uint16),
            np.resize(np.arange(4000,4301,dtype=np.uint16),(40,50)),
            np.resize(np.array([0,4000,8000,MAX],np.uint16),(40,50))]
        for fraction in [.01,.1,.49,.5,.51,.9,.99]:
            a=np.resize(np.arange(4490,4511,dtype=np.uint16),10000)
            n=round(a.size*fraction);a[:n]=np.resize(np.arange(4780,4821,dtype=np.uint16),n)
            cases.append(a.reshape(100,100))
        for n in range(12):cases.append(rng.integers(0,MAX+1,(40,50),dtype=np.uint16))
        for separation in [69,70]:
            cases.append(np.resize(np.array([4500,4500+separation],np.uint16),(40,50)))
        checked=0
        for gain in [0,1,5,20,40]:
            p=params();p.gain=gain
            for a in cases:
                out,table=apply(a,p);expected,_,meta=reference(a,p)
                assert np.abs(table.astype(int)-expected.astype(int)).max()<=1,meta
                checked+=1
        p=params();p.gain=5
        noisy=cases[3];_,table=apply(noisy,p)
        assert not reference(noisy,p)[2]['hot_segment']
        assert float(np.ptp(table[np.unique(noisy)]))*255/MAX<=11
        assert not reference(cases[-2],p)[2]['hot_segment']
        assert reference(cases[-1],p)[2]['hot_segment']
        print(f'PASS: {checked} independent Otsu/native-LUT cases; constant/noisy frames do not hot-stretch')
        # Cold/hot occupancy crossings preserve a fixed background anchor.
        background_means=[]
        for a in cases[6:13]:
            out,_=apply(a,p);mask=a<4600;background_means.append(float(out[mask].mean()*255/MAX))
        assert np.ptp(background_means)<1
        report['background_occupancy_mean_gray']=background_means
        # All temporal behavior is the one existing Q8 LUT blend.
        for smooth in [0,50,90,99]:
            p=params();p.smooth=smooth;p.gain=5;history=None
            for k in range(20):
                a=cases[3]+(380 if k<8 else 0)
                out,table=apply(a,p,reset=k==0)
                expected,history,_=reference(a,p,history)
                assert np.abs(table.astype(int)-expected.astype(int)).max()<=1
            api.agc_weighted_state_reset(ws)
            _,table=apply(cases[3],p,reset=False)
            np.testing.assert_array_equal(table,reference(cases[3],p)[0])
        p=params();p.smooth=0
        apply(cases[3]+380,p);actual=apply(cases[3],p,reset=False)[0]
        np.testing.assert_array_equal(actual,apply(cases[3],p)[0])
        np.testing.assert_array_equal(actual,apply(cases[3],p,inplace=True)[0])
        # Unused Classic controls must neither change balanced output nor reset Smooth.
        p=params();p.smooth=50;p.gain=5
        _,table=apply(cases[3],p);_,history,_=reference(cases[3],p)
        p.clip=1000;p.redistribute=0;p.bright=1e300;p.percentile=1000
        _,table=apply(cases[3]+50,p,reset=False)
        expected,_,_=reference(cases[3]+50,p,history)
        np.testing.assert_array_equal(table,expected)
        print('PASS: Smooth0/50/90/99, delayed scene, calibration reset, in-place and inactive Classic controls')
        if old:
            for a in cases:
                p=params();p.balanced=0;p.smooth=50
                for k in range(3):
                    np.testing.assert_array_equal(apply(a,p,reset=k==0)[0],
                        apply(a,p,reset=k==0,engine=old,state=old_ws)[0])
            print('PASS: Classic output bit-exact to pre-change production source')
        if args.raw_dir:
            report['raws']=[]
            for path in sorted(args.raw_dir.glob('*.raw')):
                a=np.fromfile(path,dtype='<u2').reshape(480,640)
                p=params();p.gain=5
                out,table=apply(a,p);expected,_,meta=reference(a,p)
                assert np.abs(table.astype(int)-expected.astype(int)).max()<=1
                meta.update(source=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    native_reference_max_dn=int(np.abs(table.astype(int)-expected.astype(int)).max()),
                    background_roi_mean_gray=float(out[20:120,20:220].mean()*255/MAX))
                report['raws'].append(meta)
                if args.output_dir:
                    from PIL import Image,ImageDraw
                    args.output_dir.mkdir(parents=True,exist_ok=True)
                    out.astype('<u2').tofile(args.output_dir/(path.stem+'_Otsu_RAW14.raw'))
                    rendered=np.floor(out.astype(float)*255/MAX+.5).astype(np.uint8)
                    Image.fromarray(rendered).save(args.output_dir/(path.stem+'_Otsu.png'))
                    if old:
                        prior=apply(a,p,engine=old,state=old_ws)[0]
                        image=Image.new('RGB',(640,1008),'white');d=ImageDraw.Draw(image)
                        d.text((8,5),'Before: R4',fill='black');d.text((8,509),'After: Otsu R5',fill='black')
                        image.paste(Image.fromarray(np.floor(prior.astype(float)*255/MAX+.5).astype(np.uint8)),(0,24))
                        image.paste(Image.fromarray(rendered),(0,528));image.save(args.output_dir/(path.stem+'_comparison.png'))
            print('PASS: all supplied RAWs match independent reference; outputs saved')
        report['reference_cases']=checked;report['workspace_bytes']=api.size()
        report['validation']='host only; SDK linking and board not performed'
        if args.output_dir:
            args.output_dir.mkdir(parents=True,exist_ok=True)
            (args.output_dir/'report.json').write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf-8')
        api.release(ws)
        if old:old.release(old_ws)
        if os.name=='nt':
            import _ctypes
            for lib in loaded:_ctypes.FreeLibrary(lib._handle)
    if dll_dir:dll_dir.close()

if __name__=='__main__':main()
