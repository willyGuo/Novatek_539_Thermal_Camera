"""Independent native RAW14 Weighted Mapping checks; host only."""
import argparse
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import numpy as np

N, MAX = 16384, 16383
ROOT = Path(__file__).resolve().parents[1]

class Params(ct.Structure):
    _fields_ = [(n, ct.c_double if n == 'bright' else ct.c_uint32) for n in
                ('clip','redistribute','bright','percentile','gain','smooth','simple')]

def rnd(v, divisor=1):
    v = np.asarray(v, dtype=np.int64)
    return np.where(v < 0, -((-v+divisor//2)//divisor), (v+divisor//2)//divisor)

def classic(a, p):
    h = np.bincount(np.minimum(a.ravel(), MAX), minlength=N).astype(np.int64)
    occupied = np.flatnonzero(h)
    lut = np.zeros(N, np.int64)
    if len(occupied) <= 1:
        return lut
    hist = h.copy()
    if p.clip:
        cap = max(1, min(len(a.ravel()), round(a.size*p.clip/(len(occupied)*10))))
        excess = int(np.maximum(hist-cap, 0).sum())
        hist = np.minimum(hist, cap)
        if p.redistribute:
            excess = min(excess, int((cap-hist[occupied]).sum()))
            while excess:
                bins = occupied[hist[occupied] < cap]
                batch, rem = divmod(excess, len(bins))
                additions = np.minimum(batch, cap-hist[bins])
                if rem:
                    selected = np.arange(rem)*len(bins)//rem
                    additions[selected] = np.minimum(additions[selected]+1, cap-hist[bins[selected]])
                hist[bins] += additions
                excess -= int(additions.sum())
    values = np.repeat(np.arange(N), hist)
    # Strict inverse CDF, with P100 clamped to the last retained sample.
    threshold = int(values[min(len(values)*p.percentile//1000, len(values)-1)])
    first, last = occupied[0], occupied[-1]
    weights = (hist[first:last]+hist[first+1:last+1]).astype(np.float64)
    if p.bright > 1e100:
        weights *= np.where(np.arange(first,last) >= threshold, 1, 1/p.bright)
    else:
        weights *= np.where(np.arange(first,last) >= threshold, p.bright, 1)
    lut[first+1:last+1] = np.rint(np.cumsum(weights)*MAX/weights.sum()).astype(np.int64)
    lut[last+1:] = MAX
    return lut

def limited(lut,a,p,previous=None,offset_previous=None):
    q8 = lut.astype(np.int64)*256
    if previous is not None and p.smooth:
        q8 = (q8*(100-p.smooth)+previous*p.smooth+50)//100
    pure = q8.copy()
    offset = None
    if p.gain:
        cap = round(p.gain*MAX*256/2550)
        q8 = np.r_[0,np.cumsum(np.minimum(np.diff(pure),cap))]
        difference = (pure-q8)[np.minimum(a,MAX)]
        offset = (int(difference.sum())+a.size//2)//a.size
        if offset_previous is not None and p.smooth:
            offset = (offset*(100-p.smooth)+offset_previous*p.smooth+50)//100
        q8 = np.clip(q8+offset,0,MAX*256)
    return q8,pure,offset

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc',default='gcc')
    parser.add_argument('--toolkit',type=Path)
    parser.add_argument('--raw-dir',type=Path)
    parser.add_argument('--output-dir',type=Path)
    args = parser.parse_args()
    cc = shutil.which(args.cc)
    if not cc: raise SystemExit('compiler not found')
    env = os.environ.copy();env['PATH']=str(Path(cc).parent)+os.pathsep+env.get('PATH','')
    dll_dir = os.add_dll_directory(str(Path(cc).parent)) if os.name=='nt' else None
    report={'scope':'Host numerical tests; no SDK linking, board FPS or image-quality proof',
            'sources':{n:hashlib.sha256((ROOT/'Enhancement/AGC'/n).read_bytes()).hexdigest()
                       for n in ['agc_weighted.c','agc_weighted.h']}}
    with tempfile.TemporaryDirectory(prefix='weighted_u14_',ignore_cleanup_errors=True) as tmp:
        work=Path(tmp); wrapper=work/'wrapper.c'
        wrapper_text='''#include <stdlib.h>
#include <string.h>
#include "agc_weighted.h"
#include "agc_mode.h"
AGC_MODE agc_mode_get(void){return AGC_MODE_WEIGHTED_CPU;}
void *allocate(void){return calloc(1,sizeof(AGC_WEIGHTED_WORKSPACE));}
void release(void *p){free(p);}
size_t size(void){return sizeof(AGC_WEIGHTED_WORKSPACE);}
void lut(AGC_WEIGHTED_WORKSPACE *p,void *out){memcpy(out,p->lut,sizeof(p->lut));}
'''
        wrapper.write_text(wrapper_text,encoding='utf-8')
        library=work/('test.dll' if os.name=='nt' else 'test.so')
        subprocess.run([cc,'-shared','-fPIC','-std=c99','-O2','-Wall','-Wextra','-Werror',
            '-I',str(ROOT/'Enhancement/AGC'),str(wrapper),str(ROOT/'Enhancement/AGC/agc_weighted.c'),
            '-pthread','-lm','-o',str(library)],check=True,env=env)
        lib=ct.CDLL(str(library));lib.allocate.restype=ct.c_void_p;lib.size.restype=ct.c_size_t
        lib.release.argtypes=[ct.c_void_p];lib.lut.argtypes=[ct.c_void_p,ct.c_void_p]
        lib.agc_weighted_state_reset.argtypes=[ct.c_void_p]
        lib.agc_weighted_default_params.argtypes=[ct.POINTER(Params)]
        lib.agc_weighted_apply_u14.argtypes=[ct.c_void_p,ct.c_size_t,ct.c_void_p,ct.c_size_t,
            ct.c_uint32,ct.c_uint32,ct.c_void_p,ct.POINTER(Params)]
        ws=lib.allocate(); assert ws
        def params():
            p=Params();lib.agc_weighted_default_params(ct.byref(p));p.smooth=0;p.gain=0
            return p
        def apply(a,p,reset=True,inplace=False):
            if reset:lib.agc_weighted_state_reset(ws)
            a=np.array(a,dtype=np.uint16,order='C',copy=True); original=a.copy()
            out=a if inplace else np.zeros_like(a)
            assert lib.agc_weighted_apply_u14(a.ctypes.data,a.strides[0],out.ctypes.data,out.strides[0],
                a.shape[1],a.shape[0],ws,ct.byref(p))==0
            table=np.empty(N,np.uint16);lib.lut(ws,table.ctypes.data)
            if not inplace:np.testing.assert_array_equal(a,original)
            return out.copy(),table
        # Hand-derived integer-CDF boundaries, including exact CDF equality,
        # empty DN102, P0/P100 and the former interpolated P80=103.4 case.
        boundary=np.array([[100,101,103,104]],np.uint16)
        for percentile,weights in [(0,[6,3,3,6]),(250,[2,3,3,6]),
                                   (500,[2,1,1,6]),(800,[2,1,1,2]),
                                   (1000,[2,1,1,2])]:
            p=params();p.clip=0;p.bright=3;p.percentile=percentile
            _,table=apply(boundary,p)
            expected=np.r_[0,np.rint(np.cumsum(weights)*MAX/sum(weights))].astype(np.uint16)
            np.testing.assert_array_equal(table[100:105],expected)
        print('PASS: integer Bright threshold, strict CDF boundary, empty bin and P0/P100')
        rng=np.random.default_rng(20261110)
        cases=[np.arange(N,dtype=np.uint16).reshape(128,128),
            np.array([[1000,1001,1002,1003]],np.uint16),
            np.array([[0,0,4095,4096,8191,8192,16383,65535]],np.uint16),
            np.zeros((1,1),np.uint16),np.full((11,9),8000,np.uint16)]
        cases += [rng.integers(7000,7100,(9,17),dtype=np.uint16) for _ in range(16)]
        cases += [rng.integers(0,N,(13,19),dtype=np.uint16) for _ in range(8)]
        if args.raw_dir:
            files=[f for f in sorted(args.raw_dir.rglob('*.raw')) if f.stat().st_size==614400][:14]
            cases += [np.fromfile(f,dtype='<u2').reshape(480,640) for f in files]
            report['real_raws']=[{'path':str(f),'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in files]
        checks=0
        for a in cases:
            p=params()
            for bright in [1,1.7,1e300,np.finfo(float).max]:
                p.bright=bright
                curve=classic(a,p)
                actual,table=apply(a,p)
                assert np.max(np.abs(table.astype(np.int64)-curve))<=1
                np.testing.assert_array_equal(actual,table[np.minimum(a,MAX)])
                assert np.diff(table.astype(np.int64)).min()>=0
                np.testing.assert_array_equal(apply(a,p,inplace=True)[0],actual)
                checks+=1
        p=params(); ramp=cases[0]
        out,table=apply(ramp,p)
        np.testing.assert_array_equal(out,ramp)
        assert len(np.unique(out))==N # old Gray8 implementation had only 256 levels
        skewed=np.repeat([100,102,104],[100,5,1]).reshape(2,53).astype(np.uint16)
        # With redistribution, equal retained counts at DN100/102/104 put
        # strict P70 at DN104, so no interval below DN104 is bright-weighted.
        for redistribute,expected in [(0,[0,3641,7281,14563,16383]),(1,[0,4096,8192,12287,16383])]:
            p=params();p.clip=1;p.redistribute=redistribute;p.bright=2
            np.testing.assert_array_equal(apply(skewed,p)[1][100:105],expected)
        print(f'PASS: {checks} independent Weighted curves; identity has 16384 levels')
        report['curve_checks']=checks
        for gain in (1,5,20,40):
            p=params();p.gain=gain;p.bright=1.2
            for a in cases[:12]:
                curve=classic(a,p);q8,_,_=limited(curve,a,p)
                expected=((q8+128)//256).astype(np.uint16)
                actual,table=apply(a,p)
                assert np.max(np.abs(table.astype(np.int64)-expected))<=1
                assert np.diff(table.astype(np.int64)).max()<=int(np.ceil(gain*MAX/2550))+1
        a,b=cases[5],cases[6]+11
        p=params();p.gain=20;p.smooth=99
        history=offset=None
        for k in range(20):
            frame=a if k%2==0 else b
            curve=classic(frame,p)
            q8,history,offset=limited(curve,frame,p,history,offset)
            _,table=apply(frame,p,reset=k==0)
            assert np.max(np.abs(table.astype(np.int64)-(q8+128)//256))<=1
        np.testing.assert_array_equal(apply(a,p)[0],apply(a,p)[0])
        p.gain=5
        changed=apply(a,p,reset=False)[0]
        np.testing.assert_array_equal(changed,apply(a,p)[0])
        print('PASS: Gain Limit unit conversion, mean compensation, Q8 smoothing and parameter reset')
        # Parameter padding is not a setting and must not reset temporal history.
        p=params();p.smooth=90;p.gain=20;p.bright=1.2
        first_frame,second_frame=cases[5],cases[6]
        q8,history,offset=limited(classic(first_frame,p),first_frame,p)
        apply(first_frame,p)
        tail=Params.simple.offset+ct.sizeof(ct.c_uint32)
        if ct.sizeof(p)>tail:
            ct.memset(ct.addressof(p)+tail,0xA5,ct.sizeof(p)-tail)
        q8,_,_=limited(classic(second_frame,p),second_frame,p,history,offset)
        actual,table=apply(second_frame,p,reset=False)
        assert np.max(np.abs(table.astype(np.int64)-(q8+128)//256))<=1
        print('PASS: parameter padding does not reset LUT/Gain temporal history')
        a=np.full((100,100),10000,np.uint16);b=a.copy()
        a.flat[:4900]=10001;b.flat[:5100]=10001
        for gain in (1,5,20,40):
            for smooth in (0,90,99):
                p=params();p.gain=gain;p.smooth=smooth;p.bright=1.2
                means=[]
                for k in range(20):
                    actual,_=apply(a if k%2==0 else b,p,reset=k==0)
                    means.append(actual.mean()*255/MAX)
                assert max(abs(np.diff(means))) <= (6 if smooth==0 else 2)
                assert min(means)>120 and max(means)<135
        print('PASS: low-contrast 49:51 / 51:49 flicker regression at 12 gain/smoothing combinations')
        # Real row padding and invalid calls must remain untouched.
        a=np.full((13,25),65535,np.uint16);a[:,:19]=rng.integers(0,N,(13,19),dtype=np.uint16)
        p=params();out=np.full_like(a,54321);lib.agc_weighted_state_reset(ws)
        assert lib.agc_weighted_apply_u14(a.ctypes.data,a.strides[0],out.ctypes.data,out.strides[0],19,13,ws,ct.byref(p))==0
        np.testing.assert_array_equal(out[:,:19],apply(a[:,:19],p)[0]);assert np.all(out[:,19:]==54321)
        # The old spatial-guide size bound is gone: mapping supports larger frames.
        a=rng.integers(0,N,(481,641),dtype=np.uint16)
        actual,table=apply(a,p)
        np.testing.assert_array_equal(actual,table[a])
        state=ct.string_at(ws,lib.size())
        a=np.zeros((2,3),np.uint16);out=np.full_like(a,1234)
        for name,value in [('bright',float('nan')),('bright',float('inf')),('gain',41),('percentile',1001)]:
            invalid=Params.from_buffer_copy(p);setattr(invalid,name,value)
            assert lib.agc_weighted_apply_u14(a.ctypes.data,a.strides[0],out.ctypes.data,out.strides[0],3,2,ws,ct.byref(invalid))!=0
            assert ct.string_at(ws,lib.size())==state and np.all(out==1234)
        assert lib.size()<230000
        report['workspace_bytes']=lib.size()
        print(f'PASS: stride padding, RAW input preserved, large frame support, workspace {lib.size()} bytes; no target proof')
        lib.release(ws)
        if os.name=='nt':
            import _ctypes
            _ctypes.FreeLibrary(lib._handle)
    if dll_dir:dll_dir.close()
    if args.toolkit:print('NOTE: Toolkit Gray8 output is a different quantization domain; use the native RAW14 references above.')
    if args.output_dir:
        args.output_dir.mkdir(parents=True,exist_ok=True)
        destination=args.output_dir/'weighted_raw14_report.json'
        destination.write_text(json.dumps(report,indent=2),encoding='utf-8')
        print(destination)

if __name__=='__main__':main()
