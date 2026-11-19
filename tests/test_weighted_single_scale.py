"""Native RAW14 single-scale host reference, lifecycle and optional RAW report.

No SDK link, board timing, flashing or target image proof is implied.
"""
import argparse
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import numpy as np
import cv2
from test_weighted_raw14 import Params, MAX

ROOT=Path(__file__).resolve().parents[1]


def guided_reference(image,epsilon):
    center=image.mean();x=image-center
    def box(v):return cv2.boxFilter(v,cv2.CV_64F,(49,49),borderType=cv2.BORDER_REFLECT_101)
    mean=box(x);variance=np.maximum(box(x*x)-mean*mean,0)
    a=variance/(variance+epsilon*epsilon);b=mean-a*mean
    return box(a)*x+box(b)+center


def fast_guided_reference(image,epsilon):
    centered=image-image.mean()
    h,w=image.shape
    ys,xs=np.arange(0,h,4),np.arange(0,w,4)
    sizes=np.diff(np.r_[ys,h])[:,None]*np.diff(np.r_[xs,w])
    def pool(v):
        return np.add.reduceat(np.add.reduceat(v,ys,axis=0),xs,axis=1)/sizes
    def box(v):
        return cv2.boxFilter(v,cv2.CV_64F,(13,13),borderType=cv2.BORDER_REFLECT_101)
    mean=box(pool(centered))
    variance=np.maximum(box(pool(centered**2))-mean**2,0)
    slope=variance/(variance+epsilon**2)
    gx,gy=np.meshgrid((np.arange(w,dtype=np.float32)-1.5)/4,(np.arange(h,dtype=np.float32)-1.5)/4)
    def up(v):
        return cv2.remap(box(v),gx,gy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    return up(slope)*centered+up((1-slope)*mean)+image.mean()


def reference(raw,weighted_native,mode):
    x=np.array([0,12,28,60,90,130,170,200,255],float)
    y=np.array([0,22,48,89,115,143,170,200,255],float)
    baseline=np.interp(5+206*weighted_native.astype(float)/MAX,x,y)
    # Firmware composes the brightness curve into its native RAW14 LUT first.
    baseline=np.floor(baseline*MAX/255+.5)*255/MAX
    if mode==5:
        image=np.minimum(raw,MAX).astype(float)
        h,w=image.shape
        ys,xs=np.arange(0,h,4),np.arange(0,w,4)
        sizes=np.diff(np.r_[ys,h])[:,None]*np.diff(np.r_[xs,w])
        guide=np.add.reduceat(np.add.reduceat(image,ys,axis=0),xs,axis=1)/sizes
        mean=cv2.boxFilter(guide,cv2.CV_64F,(13,13),borderType=cv2.BORDER_REFLECT_101)
        gx,gy=np.meshgrid((np.arange(w,dtype=np.float32)-1.5)/4,(np.arange(h,dtype=np.float32)-1.5)/4)
        background=cv2.remap(np.floor(mean+.5),gx,gy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
        detail=image-np.floor(background+.5)
        low,high=np.percentile(image,[1,99])
        gain=np.floor(min(.8,510/max(high-low,32))*MAX/255*256+.5)
        base=np.rint(baseline*MAX/255)*256
        limit,lo,hi=np.floor(np.array([32,3,246])*MAX/255*256+.5)
        return np.floor((np.clip(base+np.clip(gain*detail,-limit,limit),lo,hi)+128)/256).astype(np.uint16)
    if mode>=2:
        image=np.minimum(raw,MAX).astype(float)
        low,high=np.percentile(image,[1,99]);span=max(high-low,32)
        epsilon=max(2,.12*span)
        gain=min(1.2,765/span) if mode==3 else min(.8,510/span)
        limit=44 if mode==3 else 32
        guided=fast_guided_reference if mode==4 else guided_reference
        baseline=np.clip(baseline+np.clip(gain*(image-guided(image,epsilon)),-limit,limit),3,246)
    return np.floor(baseline*MAX/255+.5).astype(np.uint16)


def display(a):return np.floor(a.astype(float)*255/MAX+.5).astype(np.uint8)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--cc',default='gcc')
    parser.add_argument('--before-dir',type=Path)
    parser.add_argument('--raw-dir',type=Path)
    parser.add_argument('--raw',type=Path,action='append',default=[])
    parser.add_argument('--toolkit',type=Path)
    parser.add_argument('--output-dir',type=Path)
    args=parser.parse_args()
    cc=shutil.which(args.cc);assert cc
    env=os.environ.copy();env['PATH']=str(Path(cc).parent)+os.pathsep+env.get('PATH','')
    dll_dir=os.add_dll_directory(str(Path(cc).parent)) if os.name=='nt' else None
    report=dict(scope='Host C/NumPy reference and RAW14 image exports only; no SDK/board/FPS proof',cases=[],
        sources={n:hashlib.sha256((ROOT/'Enhancement/AGC'/n).read_bytes()).hexdigest() for n in ['agc_weighted.c','agc_weighted.h']})
    if args.toolkit:
        sys.path.insert(0,str(args.toolkit.resolve()))
        from thermal_toolkit.algorithms.tone_mapping import apply_tone_mapping
    with tempfile.TemporaryDirectory(prefix='weighted_local_',ignore_cleanup_errors=True) as directory:
        work=Path(directory);libs=[]
        def build(source,name,current):
            wrapper=work/(name+'.c')
            release='agc_weighted_workspace_release(p);free(p);' if current else 'free(p);'
            wrapper.write_text('''#include <stdlib.h>
#include <stdint.h>
#include "agc_weighted.h"
#include "agc_mode.h"
AGC_MODE agc_mode_get(void){return AGC_MODE_WEIGHTED_CPU;}
static int allocation_failure;
void fail_allocation(int value){allocation_failure=value;}
void *weighted_test_realloc(void *p,size_t n){return allocation_failure?NULL:realloc(p,n);}
void *allocate(void){return calloc(1,sizeof(AGC_WEIGHTED_WORKSPACE));}
void release(void *p){'''+release+'''}
size_t size(void){return sizeof(AGC_WEIGHTED_WORKSPACE);}
'''+('''void *buffer(AGC_WEIGHTED_WORKSPACE *p){return p->local_buffer;}
size_t capacity(AGC_WEIGHTED_WORKSPACE *p){return p->local_capacity_pixels;}
''' if current else ''),encoding='utf-8')
            obj=work/(name+'.o');dll=work/(name+('.dll' if os.name=='nt' else '.so'))
            common=[cc,'-std=c99','-O2','-Wall','-Wextra','-Werror','-fPIC','-I',str(source),'-I',str(ROOT/'Enhancement/AGC')]
            subprocess.run(common+(['-Drealloc=weighted_test_realloc'] if current else [])+
                           ['-c',str(source/'agc_weighted.c'),'-o',str(obj)],check=True,env=env)
            subprocess.run(common+['-shared',str(wrapper),str(obj),'-pthread','-lm','-o',str(dll)],check=True,env=env)
            lib=ct.CDLL(str(dll));libs.append(lib)
            lib.allocate.restype=ct.c_void_p;lib.release.argtypes=[ct.c_void_p]
            lib.size.restype=ct.c_size_t
            lib.agc_weighted_default_params.argtypes=[ct.POINTER(Params)]
            lib.agc_weighted_state_reset.argtypes=[ct.c_void_p]
            lib.agc_weighted_apply_u14.argtypes=[ct.c_void_p,ct.c_size_t,ct.c_void_p,ct.c_size_t,
                ct.c_uint32,ct.c_uint32,ct.c_void_p,ct.POINTER(Params)]
            if current:
                lib.buffer.argtypes=[ct.c_void_p];lib.buffer.restype=ct.c_void_p
                lib.capacity.argtypes=[ct.c_void_p];lib.capacity.restype=ct.c_size_t
                lib.agc_weighted_workspace_release.argtypes=[ct.c_void_p]
            return lib
        lib=build(ROOT/'Enhancement/AGC','current',True)
        old=build(args.before_dir,'previous',False) if args.before_dir else None
        ws=lib.allocate();assert ws
        old_ws=old.allocate() if old else None
        p=Params();lib.agc_weighted_default_params(ct.byref(p));p.gain=0;p.smooth=0
        def apply(raw,mode,reset=True,inplace=False,api=lib,workspace=ws):
            a=np.ascontiguousarray(raw,dtype=np.uint16)
            out=a.copy() if inplace else np.empty_like(a)
            if inplace:a=out
            p.simple=mode
            if reset:api.agc_weighted_state_reset(workspace)
            status=api.agc_weighted_apply_u14(a.ctypes.data,a.strides[0],out.ctypes.data,out.strides[0],
                                            a.shape[1],a.shape[0],workspace,ct.byref(p))
            assert status==0
            return out
        rng=np.random.default_rng(71)
        frames=[np.full((1,1),8000,np.uint16),np.full((7,3),12000,np.uint16),
                rng.integers(0,16384,(33,47),dtype=np.uint16),
                rng.integers(4500,4540,(51,39),dtype=np.uint16),
                rng.integers(4500,4540,(1,25),dtype=np.uint16),
                rng.integers(4500,4540,(49,49),dtype=np.uint16),
                rng.integers(4500,4540,(50,65),dtype=np.uint16),
                rng.integers(4500,4540,(53,67),dtype=np.uint16)]
        # Off and Brightness Only must not allocate spatial memory.
        for mode in [0,1]:
            apply(frames[2],mode);assert not lib.buffer(ws)
        # Allocation failure leaves both output and LUT history untouched.
        state=ct.string_at(ws,lib.size());a=frames[2];out=np.full_like(a,1234)
        p.simple=5;lib.fail_allocation(1)
        assert lib.agc_weighted_apply_u14(a.ctypes.data,a.strides[0],out.ctypes.data,out.strides[0],47,33,ws,ct.byref(p))<0
        assert np.all(out==1234) and ct.string_at(ws,lib.size())==state
        lib.fail_allocation(0)
        apply(a,5);assert lib.capacity(ws)==((47+3)//4)*((33+3)//4)
        p.simple=2;unchanged=a.copy()
        assert lib.agc_weighted_apply_u14(a.ctypes.data,a.strides[0],a.ctypes.data,a.strides[0]+2,
                                        a.shape[1],a.shape[0],ws,ct.byref(p))<0
        np.testing.assert_array_equal(a,unchanged)
        for a in frames:
            original=a.copy();native=apply(a,0)
            if old:np.testing.assert_array_equal(native,apply(a,0,api=old,workspace=old_ws))
            for mode in [1,2,3,4,5]:
                actual=apply(a,mode);expected=reference(a,native,mode)
                gray_error=np.max(np.abs(actual.astype(float)-expected.astype(float)))*255/MAX
                assert gray_error<.1,gray_error
                np.testing.assert_array_equal(actual,apply(a,mode,inplace=True))
                np.testing.assert_array_equal(a,original)
        # A full frame contains enough rounding ties to exercise the pooled
        # float vs double reference: at most one background DN may differ.
        yy,xx=np.indices((480,640))
        scene=(8200+xx//4+yy//9+659*((xx-318)**2+(yy-245)**2<90**2)
               +rng.integers(-16,17,(480,640))).astype(np.uint16)
        actual=apply(scene,5)
        expected=reference(scene,apply(scene,0),5)
        assert np.any(actual!=apply(scene,1))
        np.testing.assert_array_less(np.abs(actual.astype(int)-expected.astype(int)),53)
        capacity=lib.capacity(ws);pointer=lib.buffer(ws)
        apply(frames[-1],2);assert lib.buffer(ws)==pointer and lib.capacity(ws)==capacity
        # Mode transitions invalidate LUT history, while reusing spatial memory.
        p.smooth=90;p.gain=20
        for mode in [0,2,4,5,3,1,0]:
            changed=apply(frames[-1],mode,reset=False)
            np.testing.assert_array_equal(changed,apply(frames[-1],mode))
        p.gain=p.smooth=0
        # Padded rows and clipped input values must not alter row padding.
        padded=np.full((13,29),65535,np.uint16);padded[:,:21]=rng.integers(0,16384,(13,21),dtype=np.uint16)
        for mode in [1,2,3,4,5]:
            dest=np.full_like(padded,54321);p.simple=mode;lib.agc_weighted_state_reset(ws)
            assert lib.agc_weighted_apply_u14(padded.ctypes.data,padded.strides[0],dest.ctypes.data,dest.strides[0],21,13,ws,ct.byref(p))==0
            np.testing.assert_array_equal(dest[:,:21],apply(padded[:,:21],mode));assert np.all(dest[:,21:]==54321)
        # No intermediate Gray8 quantization: native brightness map has >256 levels.
        ramp=np.arange(16384,dtype=np.uint16).reshape(128,128);p.clip=0
        assert np.unique(apply(ramp,1)).size>256
        p.clip=20
        p.simple=6;out=np.full_like(frames[0],1234)
        assert lib.agc_weighted_apply_u14(frames[0].ctypes.data,2,out.ctypes.data,2,1,1,ws,ct.byref(p))<0
        assert out[0,0]==1234
        print('PASS: local C/reference, Off baseline, no Gray8 intermediate, allocation failure/reuse/release, in-place/stride/reset/validation')
        paths=(list(args.raw_dir.glob('*.raw')) if args.raw_dir else [])+args.raw
        if args.output_dir:args.output_dir.mkdir(parents=True,exist_ok=True)
        for path in paths:
            a=np.fromfile(path,dtype='<u2').reshape(480,640);before=hashlib.sha256(path.read_bytes()).hexdigest()
            p.gain=p.smooth=0;native=apply(a,0)
            if old:np.testing.assert_array_equal(native,apply(a,0,api=old,workspace=old_ws))
            for mode,name in [(1,'brightness'),(2,'mild'),(3,'strong')]:
                actual=apply(a,mode);expected=reference(a,native,mode)
                diff=np.abs(actual.astype(float)-expected.astype(float))*255/MAX
                assert diff.max()<.1,(path,mode,diff.max())
                record=dict(file=str(path),mode=name,raw_sha256=before,
                            c_reference_max_error_gray=float(diff.max()),spatial_scratch_bytes=int(lib.capacity(ws)*16))
                if args.toolkit:
                    toolkit,*_=apply_tone_mapping(a,'histogram_weighted',16383,
                        {'histogram_weighted_simple_mode':name,'histogram_weighted_gain_limit':0})
                    display_error=np.abs(display(actual).astype(int)-toolkit.astype(int))
                    record.update(toolkit_display_max_error_gray=int(display_error.max()),
                                  toolkit_display_mae_gray=float(display_error.mean()))
                    assert display_error.max()<=2,(path,name,display_error.max())
                report['cases'].append(record)
                if args.output_dir:
                    from PIL import Image
                    actual.astype('<u2').tofile(args.output_dir/(path.stem+'_'+name+'_RAW14.raw'))
                    Image.fromarray(display(actual)).save(args.output_dir/(path.stem+'_'+name+'.png'))
            assert hashlib.sha256(path.read_bytes()).hexdigest()==before
        report.update(raw_files=len(paths),workspace_bytes=lib.size(),spatial_planes=4,
                      worst_c_reference_gray=max((r['c_reference_max_error_gray'] for r in report['cases']),default=0))
        lib.agc_weighted_workspace_release(ws);assert not lib.buffer(ws) and not lib.capacity(ws)
        lib.agc_weighted_workspace_release(ws);lib.release(ws)
        if old:old.release(old_ws)
        if args.output_dir:(args.output_dir/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print('PASS:',len(paths),'RAW files;',len(report['cases']),'mode cases; host only')
        if os.name=='nt':
            import _ctypes
            for api in libs:_ctypes.FreeLibrary(api._handle)
    if dll_dir:dll_dir.close()


if __name__=='__main__':main()
