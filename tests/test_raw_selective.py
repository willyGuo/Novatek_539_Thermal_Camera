"""Selective-neighbor quality and paired host cost, using production C.
No board FPS, real-scene ghosting or sensor FPN calibration proof.
"""
import argparse
import ctypes as ct
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import numpy as np
from raw_denoise_sources import RAW_DENOISE_SOURCES

parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
cc=shutil.which('gcc');assert cc
env=os.environ.copy();env['PATH']=str(Path(cc).parent)+os.pathsep+env.get('PATH','')

class Params(ct.Structure):
    _fields_=[(n,ct.c_uint32) for n in ('enable','threshold','shift','spatial','edge',
        'dual','fast','raw','hysteresis','hold','spatial_mode')]

class State(ct.Structure):
    _fields_=[('history',ct.c_void_p),('capacity',ct.c_size_t),('width',ct.c_uint32),
        ('height',ct.c_uint32),('time',ct.c_uint64),('valid',ct.c_int),('params',Params),
        ('slow',ct.c_void_p),('motion',ct.c_void_p),('dual_capacity',ct.c_size_t),
        ('row',ct.c_void_p),('row_capacity',ct.c_size_t)]

def params(mode,dual=1):return Params(1,12,2,1,8,dual,3,8,1,3,mode)

results={'boundary':'Host production C, GCC -O2; no HDAL/other ISP/encoder/cache costs; synthetic data only',
         'noise':[],'timing':[],'edge':[]}
dll_dir=os.add_dll_directory(str(Path(cc).parent)) if os.name=='nt' else None
with tempfile.TemporaryDirectory(prefix='raw_selective_') as temp:
    dll=Path(temp)/'filter.dll'
    subprocess.run([cc,'-std=c99','-O2','-Wall','-Wextra','-Werror','-D__LINUX','-shared','-fPIC',
        *RAW_DENOISE_SOURCES,'-pthread','-o',str(dll)],check=True,env=env)
    lib=ct.CDLL(str(dll))
    lib.raw_temporal_denoise_process.argtypes=[ct.POINTER(State),ct.c_void_p,ct.c_size_t,
        ct.c_uint32,ct.c_uint32,ct.c_uint64,ct.POINTER(Params)]
    lib.raw_temporal_denoise_destroy.argtypes=[ct.POINTER(State)]
    def process(s,a,p):
        assert lib.raw_temporal_denoise_process(ct.byref(s),a.ctypes.data,a.strides[0],
            a.shape[1],a.shape[0],s.time+33333,ct.byref(p))>0
    rng=np.random.default_rng(539)
    frames=np.rint(10000+rng.normal(0,2.518,(640,64,96))).astype(np.uint16)
    for mode in (0,1):
        s=State();p=params(mode);samples=[];counts=np.zeros(3,dtype=np.int64)
        try:
            for i,frame in enumerate(frames):
                a=frame.copy();process(s,a,p)
                if i>=160:
                    samples.append(a[1:-1,1:-1])
                    motion=np.ctypeslib.as_array(ct.cast(s.motion,ct.POINTER(ct.c_uint16)),shape=(a.size,)).reshape(a.shape)
                    counts+=np.bincount((motion[1:-1,1:-1]>>8).ravel(),minlength=3)
            results['noise'].append(dict(mode=mode,input_sd=float(frames[160:,1:-1,1:-1].astype(float).std()),
                output_sd=float(np.std(samples)),motion_percent=(100*counts/counts.sum()).tolist()))
        finally:lib.raw_temporal_denoise_destroy(ct.byref(s))
    assert results['noise'][1]['output_sd'] < results['noise'][0]['output_sd']*0.8

    # Above-threshold sharp step and 1-pixel line retain exact contrast in
    # spatial-only initialization, in both polarities, including high DN.
    for base in (0,10000,65000):
        for contrast in (9,100):
            for line in (False,True):
                for mode in (0,1):
                    a=np.full((24,32),base,dtype=np.uint16)
                    a[:,16:17 if line else 32]+=contrast
                    original=a.copy();s=State()
                    try:
                        process(s,a,params(mode,0));np.testing.assert_array_equal(a,original)
                    finally:lib.raw_temporal_denoise_destroy(ct.byref(s))
    results['edge'].append('PASS: noiseless 9/100 DN vertical step and one-pixel line; original contrast preserved at base 0/10000/65000')

    # Real spatial response below the threshold is smoothing, not guaranteed
    # texture preservation. Record a 4-DN one-pixel line, rather than hide it.
    for mode in (0,1):
        a=np.full((24,32),10000,dtype=np.uint16);a[:,16]+=4;s=State()
        try:
            process(s,a,params(mode,0))
            results['edge'].append(dict(mode=mode,low_contrast_line_dn=a[12,14:19].astype(int).tolist()))
        finally:lib.raw_temporal_denoise_destroy(ct.byref(s))

    # Uniform low-contrast step responses must remain identical: selective
    # spatial does not alter the temporal weights/detector for uniform inputs.
    for step in (1,2,3,4,7,8,20):
        traces=[]
        for mode in (0,1):
            s=State();p=params(mode);trace=[]
            try:
                a=np.full((5,5),10000,dtype=np.uint16);process(s,a,p)
                for i in range(64):
                    a.fill(10000+step);process(s,a,p);trace.append(int(a[2,2]))
                traces.append(trace)
            finally:lib.raw_temporal_denoise_destroy(ct.byref(s))
        assert traces[0]==traces[1]
    print('PASS: selective noise reduction, rejected-edge/line contrast and unchanged uniform temporal step response')

    for h in (480,512):
        for scene in ('flat_noise','moving_edge'):
            frames=np.rint(10000+rng.normal(0,2.518,(48,h,640))).astype(np.uint16)
            if scene=='moving_edge':
                for i,a in enumerate(frames):a[:,300+i:]+=100
            states=[State(),State()];ps=[params(m) for m in (0,1)];times=[[],[]];a=np.empty_like(frames[0])
            try:
                for run in range(4):
                    for i,frame in enumerate(frames):
                        for mode in ((0,1) if run%2==0 else (1,0)):
                            np.copyto(a,frame)
                            start=time.perf_counter_ns();process(states[mode],a,ps[mode]);elapsed=time.perf_counter_ns()-start
                            if i>=8:times[mode].append(elapsed/1e6)
                results['timing'].append(dict(height=h,scene=scene,
                    strict_median_ms=float(np.median(times[0])),selective_median_ms=float(np.median(times[1])),
                    strict_p95_ms=float(np.percentile(times[0],95)),selective_p95_ms=float(np.percentile(times[1],95))))
            finally:
                for s in states:lib.raw_temporal_denoise_destroy(ct.byref(s))
    if os.name=='nt':
        import _ctypes
        _ctypes.FreeLibrary(lib._handle)
if dll_dir:dll_dir.close()
if args.output:args.output.write_text(json.dumps(results,indent=2),encoding='utf-8')
print(json.dumps(results,indent=2))
