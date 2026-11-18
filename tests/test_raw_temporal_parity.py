"""Frozen pre-optimization oracle: pixels, Q8 histories, motion and row state.

Optional --output records host-only paired kernel timing (not target FPS).
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
from raw_denoise_sources import RAW_DENOISE_DIR, RAW_DENOISE_SOURCES

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='gcc')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
cc = shutil.which(args.cc)
assert cc
env = os.environ.copy()
env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')

class Params(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in (
        'enable', 'threshold', 'shift', 'spatial', 'edge', 'dual', 'fast', 'raw', 'hysteresis', 'hold', 'spatial_mode')]

class State(ct.Structure):
    _fields_ = [('history', ct.c_void_p), ('capacity', ct.c_size_t),
        ('width', ct.c_uint32), ('height', ct.c_uint32), ('time', ct.c_uint64),
        ('valid', ct.c_int), ('params', Params), ('slow', ct.c_void_p),
        ('motion', ct.c_void_p), ('dual_capacity', ct.c_size_t),
        ('row', ct.c_void_p), ('row_capacity', ct.c_size_t)]

def process(lib, state, image, width, timestamp, params):
    return lib.raw_temporal_denoise_process(ct.byref(state), image.ctypes.data,
        image.strides[0], width, image.shape[0], timestamp, ct.byref(params))

def check_state(states):
    a, b = states
    assert (a.width,a.height,a.time,a.valid)==(b.width,b.height,b.time,b.valid)
    assert bytes(a.params)==bytes(b.params)
    for name, size in [('history',a.width*a.height*4), ('slow',a.width*a.height*4),
                       ('motion',a.width*a.height*2), ('row',a.width*2)]:
        pa,pb=getattr(a,name),getattr(b,name)
        assert bool(pa)==bool(pb)
        # Inactive retained buffers can contain older-sized data; compare only
        # the arrays written by this frame's selected path.
        active = name=='history' or (a.params.dual if name in ('slow','motion') else
                                    a.params.spatial and a.width>2 and a.height>2)
        if pa and active: assert ct.string_at(pa,size)==ct.string_at(pb,size), name

dll_dir = os.add_dll_directory(str(Path(cc).parent)) if os.name=='nt' else None
with tempfile.TemporaryDirectory(prefix='raw_parity_') as temp:
    libs=[]
    for name, sources in [('before', [str(root/'tests/fixtures/raw_temporal_reference.c')]),
                          ('after', RAW_DENOISE_SOURCES)]:
        dll=Path(temp)/(name+'.dll')
        subprocess.run([cc,'-std=c99','-O2','-Wall','-Wextra','-Werror','-D__LINUX',
            '-shared','-fPIC','-I',str(RAW_DENOISE_DIR),*sources,
            '-pthread','-o',str(dll)],check=True,env=env)
        lib=ct.CDLL(str(dll))
        lib.raw_temporal_denoise_process.argtypes=[ct.POINTER(State),ct.c_void_p,
            ct.c_size_t,ct.c_uint32,ct.c_uint32,ct.c_uint64,ct.POINTER(Params)]
        lib.raw_temporal_denoise_destroy.argtypes=[ct.POINTER(State)]
        libs.append(lib)
    rng=np.random.default_rng(539)
    comparisons=0
    for width,height in [(1,1),(1,7),(2,5),(3,3),(7,5),(32,24),(640,480),(640,512)]:
        states=[State(),State()]
        try:
            timestamp=1000000
            for spatial in (0,1):
                for dual in (0,1):
                    for shift in (1,2,3):
                        p=Params(1,21,shift,spatial,8,dual,3,8,1,3)
                        for i in range(20):
                            timestamp += 300001 if i==7 else (-100 if i==11 else (0 if i==12 else 33333))
                            if i in (2,3,4): p.edge=(0,16383,8)[i-2]
                            p.hold=255 if i in (8,9,10) else 3
                            p.enable=0 if i==15 else 1
                            # Alternating full UINT16, flat noise, moving edge,
                            # threshold-boundary steps, saturation; padded rows.
                            source=rng.integers(0,65536,(height,width+3),dtype=np.uint16)
                            if i%4:
                                source[:,:width]=np.clip(np.rint(65000+rng.normal(0,2.5,(height,width))),0,65535).astype(np.uint16)
                                if i%4==2: source[:,width//2:width]=65535
                                if i%4==3: source[:,:width]=65000+(i%9)
                            images=[source.copy(),source.copy()]
                            ret=[process(lib,s,a,width,timestamp,p) for lib,s,a in zip(libs,states,images)]
                            assert ret[0]==ret[1]
                            np.testing.assert_array_equal(*images)
                            check_state(states)
                            comparisons+=1
        finally:
            for lib,s in zip(libs,states): lib.raw_temporal_denoise_destroy(ct.byref(s))
    print(f'PASS: {comparisons} frames, exact pixels/padding/Q8 histories/motion/spatial row; tiny and both sensor layouts, mode/parameter/timestamp resets')
    results=[]
    if args.output:
        for height in (480,512):
            frames=np.rint(10000+rng.normal(0,2.518,(40,height,640))).astype(np.uint16)
            for spatial,dual in [(0,0),(1,0),(0,1),(1,1)]:
                p=Params(1,21,1,spatial,8,dual,3,8,1,3)
                timings=[[],[]];states=[State(),State()]
                image=np.empty_like(frames[0])
                try:
                    for run in range(4):
                        for i,frame in enumerate(frames):
                            for index in ((0,1) if run%2==0 else (1,0)):
                                np.copyto(image,frame)
                                start=time.perf_counter_ns()
                                assert process(libs[index],states[index],image,640,states[index].time+33333,p)>0
                                elapsed=time.perf_counter_ns()-start
                                if i>=8: timings[index].append(elapsed/1e6)
                    before,after=[float(np.median(t)) for t in timings]
                    row=dict(height=height,spatial=spatial,dual=dual,before_ms=before,after_ms=after,
                        reduction_percent=100*(1-after/before))
                    results.append(row);print(row)
                finally:
                    for lib,s in zip(libs,states): lib.raw_temporal_denoise_destroy(ct.byref(s))
        args.output.write_text(json.dumps({'boundary':'Windows host kernel only; excludes HDAL/copy/cache/other pipeline work; not NT539 FPS',
            'parity_frames':comparisons,'timings':results},indent=2),encoding='utf-8')
    if os.name=='nt':
        import _ctypes
        for lib in libs: _ctypes.FreeLibrary(lib._handle)
if dll_dir: dll_dir.close()
