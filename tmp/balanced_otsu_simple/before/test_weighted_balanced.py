"""Host-only native RAW14 balanced mapping; independent reference + Toolkit comparison."""
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
from test_weighted_raw14 import Params, ROOT, N, MAX


def reference_center(a, previous=None):
    """Expanded NumPy statistics, independent of the C histogram scan."""
    values, counts = np.unique(a, return_counts=True)
    left = np.cumsum(counts)[:-1].astype(float)
    right = a.size-left
    if not left.size:
        return float(np.median(a)), False
    weighted = np.cumsum(values.astype(float)*counts)
    delta = (weighted[-1]-weighted[:-1])/right-weighted[:-1]/left
    minimum = max(2, int(np.ceil(a.size*.003)))
    # Independent expansion into value groups and per-group sample medians.
    chunks=np.split(values,np.flatnonzero(np.diff(values)>=13)+1)
    groups=[]
    for chunk in chunks:
        sample=a[(a>=chunk[0])&(a<=chunk[-1])]
        if sample.size>=minimum:
            groups.append((sample.size,float(np.median(sample)),float(np.mean(sample))))
    if len(groups)>=2 and max(g[2] for g in groups)-min(g[2] for g in groups)>=70:
        nearby=[]
        supported=previous is not None and np.count_nonzero(a<previous-40)<int(np.ceil(a.size*.9))
        if supported:
            nearby=[g for g in groups if abs(g[1]-previous)<=40]
        if nearby:
            return min(nearby,key=lambda g:(abs(g[1]-previous),-g[0]))[1],True
        if len(groups)>=3:
            return max(groups,key=lambda g:g[0])[1],True
    eligible = (left>=minimum)&(right>=minimum)&(delta>=70)
    gaps = np.flatnonzero(eligible&(np.diff(values)-1>=12))
    if gaps.size:
        index = int(gaps[0])
    else:
        score = left*right*delta**2
        variance = float(np.var(a.astype(float)))
        eligible &= score/(a.size*a.size*variance) >= .85 if variance else False
        if not np.any(eligible):
            return float(np.median(a)), False
        index = int(np.argmax(np.where(eligible,score,-1)))
    return float(np.median(a[a<=values[index]])), True


def follow(previous, desired, smooth, maximum):
    step = min(round(abs(desired-previous)*(100-smooth)/100), maximum)
    return previous+step if desired>=previous else previous-step


def reference(a, p, weighted_helper, previous=None):
    a = np.minimum(a, MAX).astype(np.int64)
    candidate, confident = reference_center(a,previous['center']/256 if previous is not None and p.smooth else None)
    desired = int(np.floor(candidate*256+.5))
    state = dict(previous) if previous is not None and p.smooth else {}
    if state:
        current = state['center']
        hot_fallback = not confident and np.percentile(a,99.95)>=current/256+70 and np.mean(a>current/256+40)>=.003
        if hot_fallback:
            state['pending_frames']=0
        else:
            accept=True
            if abs(desired-current)>4*256:
                if not state['pending_frames'] or abs(state['pending']-desired)>4*256:
                    state['pending_frames']=1
                else:
                    state['pending_frames']=min(3,state['pending_frames']+1)
                state['pending']=desired
                accept=state['pending_frames']>=3
            else:
                state['pending_frames']=0
            if accept:state['center']=follow(current,desired,p.smooth,256)
    else:
        state=dict(center=desired,pending=desired,pending_frames=0)
    center = state['center']/256
    detected = float(np.mean(a > center + 40))
    active = detected >= .003 and np.percentile(a, 99.95) >= center + 70
    target = 112 - 20 * min(1, detected / .015) if active else 112
    desired_target=int(np.floor(target*256+.5))
    state['target'] = follow(state['target'],desired_target,p.smooth,128) if previous is not None and p.smooth else desired_target
    target=state['target']/256
    gain = min(1.2, p.gain / 10) if p.gain else 1.2
    x = np.arange(N)
    curve = target + gain * (x - center)
    shadows = x<center
    curve[shadows] = 16+(target-16)*np.exp(gain*(x[shadows]-center)/(target-16))
    knee = min(int(np.ceil(center + 20)), MAX)
    if active:
        hot = a[a > knee]
        data = {}
        weighted_helper(hot.reshape(1, -1), MAX, p.clip / 10, bool(p.redistribute),
                        p.bright, p.percentile / 10, interval_data=data)
        weights = data['adjusted_weights']
        first, last = int(hot.min()), int(hot.max())
        if last > first and weights.sum() > 0:
            # Use independently supplied interval weights, retaining native output
            # precision rather than the Toolkit's rounded Gray8 local LUT.
            local = np.zeros(N, np.int64)
            local[first+1:last+1] = np.floor(np.cumsum(weights[first:last]) / weights[first:last].sum() * MAX + .5)
            local[last+1:] = MAX
            q8 = local * 256
            if p.gain:
                cap = round(p.gain * MAX * 256 / 2550)
                limited = np.r_[0, np.cumsum(np.minimum(np.diff(q8), cap))]
                anchor = int(np.sort(hot)[(hot.size-1)//2])
                q8 = np.clip(limited + q8[anchor] - limited[anchor], 0, MAX*256)
            lo, hi = q8[knee], q8[last]
            if hi > lo:
                normalized = np.clip((q8-lo)/(hi-lo), 0, 1)
                curve[knee:] = curve[knee] + (245-curve[knee])*normalized[knee:]
    lut = np.floor(np.clip(curve*MAX/255, 0, MAX)+.5).astype(np.int64)
    q8 = lut*256
    if previous is not None and p.smooth:
        q8 = (q8*(100-p.smooth)+previous['lut']*p.smooth+50)//100
    history = q8.copy()
    if p.gain:
        cap = round(p.gain*MAX*256/2550)
        q8 = np.r_[q8[0], q8[0]+np.cumsum(np.minimum(np.diff(q8), cap))]
    state['lut']=history
    return ((q8+128)//256).astype(np.uint16), state


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--toolkit', required=True, type=Path)
    parser.add_argument('--raw-dir', type=Path)
    parser.add_argument('--before-dir', type=Path)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(args.toolkit.resolve()))
    from thermal_toolkit.algorithms.histogram import histogram_weighted_mapping_native_u8 as toolkit
    from thermal_toolkit.algorithms.weighted_aci import configure_weighted_aci
    configure_weighted_aci({'enabled': False})
    cc = shutil.which('gcc'); assert cc
    env = os.environ.copy();env['PATH'] = str(Path(cc).parent)+os.pathsep+env.get('PATH', '')
    dll_dir = os.add_dll_directory(str(Path(cc).parent)) if os.name == 'nt' else None
    report = dict(scope='Host C and numerical references only; no SDK link/board/FPS proof',
                  balanced_profile='balanced_background_v2',
                  background_tracking=dict(enabled_when='balanced_background=1 and Smooth>0',
                      confirmation_frames=3,candidate_tolerance_dn=4,
                      maximum_center_step_dn=1,maximum_target_step_gray=.5),
                  real_raws_scope='Reset history, Smooth0, Clip2/BW1/P70/Gain2; synthetic temporal stress separately uses the reported runtime settings',
                  sources={name: hashlib.sha256((ROOT/'Enhancement/AGC'/name).read_bytes()).hexdigest()
                           for name in ['agc_weighted.c', 'agc_weighted.h']}, real_raws=[])
    with tempfile.TemporaryDirectory(prefix='balanced_soc_', ignore_cleanup_errors=True) as tmp:
        work = Path(tmp)
        wrapper = work/'wrapper.c'
        wrapper.write_text('''#include <stdlib.h>
#include <string.h>
#include "agc_weighted.h"
#include "agc_mode.h"
AGC_MODE agc_mode_get(void){return AGC_MODE_WEIGHTED_CPU;}
void *allocate(void){return calloc(1,sizeof(AGC_WEIGHTED_WORKSPACE));}
void release(void *p){free(p);}
size_t size(void){return sizeof(AGC_WEIGHTED_WORKSPACE);}
void lut(AGC_WEIGHTED_WORKSPACE *p,void *out){memcpy(out,p->lut,sizeof(p->lut));}
#ifdef BALANCED_TRACKING_STATE
void tracking(AGC_WEIGHTED_WORKSPACE *p,uint32_t *out){
out[0]=p->balanced_center_q8;out[1]=p->balanced_target_q8;
out[2]=p->balanced_candidate_q8;out[3]=p->balanced_candidate_frames;
out[4]=p->balanced_background_valid;}
uint32_t reacquiring(AGC_WEIGHTED_WORKSPACE *p){return p->reacquire_remaining;}
#endif
''', encoding='utf-8')
        loaded = []
        def library(source_dir, name):
            path = work/(name+('.dll' if os.name == 'nt' else '.so'))
            subprocess.run([cc, '-shared', '-fPIC', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                *(['-DBALANCED_TRACKING_STATE'] if name=='current' else []),
                '-I', str(source_dir), '-I', str(ROOT/'Enhancement/AGC'), str(wrapper),
                str(source_dir/'agc_weighted.c'), '-pthread', '-lm', '-o', str(path)], check=True, env=env)
            lib = ct.CDLL(str(path)); loaded.append(lib)
            lib.allocate.restype = ct.c_void_p;lib.size.restype = ct.c_size_t
            lib.release.argtypes = [ct.c_void_p];lib.lut.argtypes = [ct.c_void_p, ct.c_void_p]
            lib.agc_weighted_state_reset.argtypes = [ct.c_void_p]
            lib.agc_weighted_default_params.argtypes = [ct.POINTER(Params)]
            lib.agc_weighted_validate_params.argtypes = [ct.POINTER(Params)]
            lib.agc_weighted_apply_u14.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_void_p,
                ct.c_size_t, ct.c_uint32, ct.c_uint32, ct.c_void_p, ct.POINTER(Params)]
            if name=='current':
                lib.tracking.argtypes=[ct.c_void_p,ct.c_void_p]
                lib.reacquiring.argtypes=[ct.c_void_p]
                lib.agc_weighted_begin_reacquire.argtypes=[ct.c_void_p]
            return lib
        lib = library(ROOT/'Enhancement/AGC', 'current');ws = lib.allocate();assert ws
        def params():
            p = Params();lib.agc_weighted_default_params(ct.byref(p));p.balanced=1;p.smooth=0
            return p
        def apply(a, p, reset=True, api=lib, state=ws, inplace=False):
            a = np.array(a, dtype=np.uint16, order='C', copy=True);before=a.copy()
            out = a if inplace else np.zeros_like(a)
            if reset:api.agc_weighted_state_reset(state)
            assert api.agc_weighted_apply_u14(a.ctypes.data, a.strides[0], out.ctypes.data, out.strides[0],
                a.shape[1], a.shape[0], state, ct.byref(p)) == 0
            table = np.empty(N, np.uint16);api.lut(state, table.ctypes.data)
            assert np.diff(table.astype(int)).min() >= 0
            np.testing.assert_array_equal(out, table[np.minimum(before, MAX)])
            if not inplace:np.testing.assert_array_equal(a, before)
            return out, table
        rng = np.random.default_rng(20261116)
        base = rng.integers(4490, 4511, (80, 100), dtype=np.uint16)
        cases = [np.zeros((1,1), np.uint16), np.full((7,9), MAX, np.uint16), base,
                 np.array([[0,0,1,16383,65535,65535]], np.uint16),
                 np.arange(N, dtype=np.uint16).reshape(128,128)]
        for count in [24,80,400,3000]:
            a=base.copy();a.flat[-count:]=rng.integers(4600,4900,count,dtype=np.uint16);cases.append(a)
        cases.append(np.array([[4500]*90+[4700]*10], np.uint16))
        checks = 0
        for clip, red, bright, gain in [(20,1,1.,20),(0,0,1.7,0),(1,0,2.,1),(20,1,1.7,5),(20,1,np.finfo(float).max,40)]:
            p=params();p.clip=clip;p.redistribute=red;p.bright=bright;p.gain=gain
            for a in cases:
                _,actual=apply(a,p);expected,_=reference(a,p,toolkit)
                assert np.max(np.abs(actual.astype(int)-expected.astype(int)))<=1
                checks+=1
        report['native_reference_cases']=checks
        print(f'PASS: {checks} native reference curves (constant, hot area, clipping, fractional gains, extreme Bright Weight)')
        occupancy_results=[]
        for fraction in [.01,.1,.4,.49,.5,.51,.6,.8,.9,.95,.99]:
            for gain in [0,1,20]:
                count=int(round(10000*fraction))
                a=np.resize(np.arange(4490,4511,dtype=np.uint16),10000)
                a[:count]=np.resize(np.arange(4780,4821,dtype=np.uint16),count)
                a=a.reshape(100,100);background=np.arange(10000).reshape(a.shape)>=count
                p=params();p.gain=gain
                out,table=apply(a,p);expected,_=reference(a,p,toolkit)
                assert np.max(np.abs(table.astype(int)-expected.astype(int)))<=1
                assert np.all(out[background]>0) and np.ptp(out[background])>0
                if gain==20:
                    occupancy_results.append(dict(hot_fraction=fraction,background_mean_gray=float(out[background].mean()*255/MAX),
                        background_span_gray=float(np.ptp(out[background]))*255/MAX,background_black_pixels=int(np.count_nonzero(out[background]==0))))
        # No-gap bimodal input (Otsu), a broad unimodal gradient, and one cold outlier.
        bridge=np.resize(np.arange(4700,4741,dtype=np.uint16),10000)
        bridge[:2000]=np.resize(np.arange(4590,4611,dtype=np.uint16),2000)
        bridge[2000:2089]=np.arange(4611,4700,dtype=np.uint16)
        ramp=np.resize(np.arange(4000,4301,dtype=np.uint16),(100,100));ramp.flat[0]=0
        for a in [bridge.reshape(100,100),ramp]:
            p=params();out,table=apply(a,p);expected,_=reference(a,p,toolkit)
            assert np.max(np.abs(table.astype(int)-expected.astype(int)))<=1
            assert float(out.min())*255/MAX>=15.99
        report['hot_occupancy_regression']=occupancy_results
        report['additional_v2_reference_cases']=35
        print('PASS: 35 v2 cases; hot occupancy1..99%, half-frame crossing, Otsu/no-gap, shadows and cold outlier')
        p=params();p.smooth=90;history=None
        for k in range(18):
            a=cases[2] if k%2==0 else cases[7]
            expected,history=reference(a,p,toolkit,history)
            actual,table=apply(a,p,reset=k==0)
            assert np.max(np.abs(table.astype(int)-expected.astype(int)))<=1
        p.balanced=0;after_toggle=apply(cases[7],p,reset=False)[0]
        np.testing.assert_array_equal(after_toggle,apply(cases[7],p)[0])
        p.balanced=1;after_toggle=apply(cases[7],p,reset=False)[0]
        np.testing.assert_array_equal(after_toggle,apply(cases[7],p)[0])
        np.testing.assert_array_equal(apply(cases[7],p)[0],apply(cases[7],p,inplace=True)[0])
        invalid=params();invalid.balanced=2
        assert lib.agc_weighted_validate_params(ct.byref(invalid)) != 0
        print('PASS: Q8 Smooth, switch history reset, in-place and invalid switch rejection')
        # Source anchor changes need3 consistent frames; holding a hot fallback
        # must not turn that unreliable median into a new background center.
        def tracking():
            values=np.empty(5,np.uint32);lib.tracking(ws,values.ctypes.data);return values
        p=params();p.smooth=50
        apply(np.full((20,20),4500,np.uint16),p)
        for k in range(3):
            apply(np.full((20,20),4520,np.uint16),p,reset=False)
            assert tracking()[0]==(4500 if k<2 else 4501)*256
        apply(np.full((20,20),4500,np.uint16),p)
        for k in range(10):
            apply(np.full((20,20),4800,np.uint16),p,reset=False)
            assert tracking()[0]==4500*256
        lib.agc_weighted_state_reset(ws)
        assert tracking()[4]==0 and tracking()[3]==0
        print('PASS: 3-frame group confirmation, hot-fallback hold and tracker reset')
        # FFC can expose uniform/settling frames before the real open scene.
        # A wrong initial anchor must not take hundreds of1-DN steps to recover.
        p=params();p.clip=80;p.bright=2;p.percentile=800;p.gain=5;p.smooth=50
        uniform=np.full((20,20),5000,np.uint16)
        scene=np.resize(np.arange(4490,4511,dtype=np.uint16),(20,20))
        instant=params();ct.memmove(ct.byref(instant),ct.byref(p),ct.sizeof(p));instant.smooth=0
        expected=apply(scene,instant)[0]
        apply(uniform,p)
        for _ in range(2):slow=apply(scene,p,reset=False)[0]
        slow_error=float(np.mean(np.abs(slow.astype(float)-expected))*255/MAX)
        assert slow_error>50
        lib.agc_weighted_begin_reacquire(ws)
        for _ in range(3):
            apply(uniform,p,reset=False)
            assert lib.reacquiring(ws)>0  # Uniform frames cannot finish early.
        for k in range(8):
            fast=apply(scene,p,reset=False)[0]
            np.testing.assert_array_equal(fast,expected)  # Includes the FIRST scene.
            assert tracking()[0]==4500*256
        assert lib.reacquiring(ws)==0
        # Normal anti-jump tracking resumes after successful acquisition.
        for k in range(3):
            apply(scene+20,p,reset=False)
            assert tracking()[0]==(4500 if k<2 else 4501)*256
        # Truly flat scenes finish within90 frames, even at Smooth99.
        p.smooth=99;lib.agc_weighted_begin_reacquire(ws)
        for k in range(90):
            apply(uniform,p,reset=False)
            assert lib.reacquiring(ws)==89-k
        assert tracking()[0]==5000*256
        # A request survives the first size/parameter-cache initialization.
        fresh=lib.allocate()
        lib.agc_weighted_begin_reacquire(fresh)
        apply(scene,p,reset=False,state=fresh)
        assert lib.reacquiring(fresh)==89
        lib.agc_weighted_state_reset(fresh)
        assert lib.reacquiring(fresh)==0
        lib.release(fresh)
        report['ffc_reacquire']=dict(scope='Synthetic RAW; no SDK/board proof',
            first_open_scene_max_error_dn=0,without_reacquire_error_before_detection_gray=slow_error,
            minimum_frames=8,uniform_timeout_frames=90,stable_candidates=8,
            window_center_range_dn=2,window_target_range_gray=.5)
        # Slowly settling RAW must not pass an adjacent-frame-only stability
        # check. This reproduced ~2.23 seconds of error in the previous version.
        p.smooth=50;lib.agc_weighted_begin_reacquire(ws)
        peak=0.;completion=None
        for k in range(120):
            offset=max(0,80-3*k)
            current=scene+offset
            # Separate stateless workspace preserves the production history.
            state_ref=lib.allocate()
            expected=apply(current,instant,state=state_ref)[0]
            lib.release(state_ref)
            actual=apply(current,p,reset=False)[0]
            error=float(np.abs(actual.astype(float)-expected).max())
            peak=max(peak,error)
            if completion is None and not lib.reacquiring(ws):completion=k+1
            if k<27:assert lib.reacquiring(ws)>0
        assert peak==0 and completion is not None and 27<completion<90
        report['ffc_reacquire']['slow_drift_max_error_dn']=peak
        report['ffc_reacquire']['slow_drift_acquisition_finished_frame']=completion
        # A stable center alone is insufficient if hot activation/target drifts.
        lib.agc_weighted_begin_reacquire(ws)
        base=np.resize(np.arange(4490,4511,dtype=np.uint16),40000)
        for k in range(10):
            sample=base.copy();sample[:120+k*40]=4800
            apply(sample.reshape(200,200),p,reset=False)
            assert lib.reacquiring(ws)>0
        print('PASS: cumulative center/target stability rejects slow settling; RAW drift error0 DN')
        # Actual missing case: noisy closed view passes8-frame acquisition,
        # then the real scene arrives AFTER acquisition has already finished.
        # Recovery must not depend on a GPIO/FFC event arriving at that moment.
        lib.agc_weighted_begin_reacquire(ws)
        for _ in range(8):apply(scene+380,p,reset=False)
        assert lib.reacquiring(ws)==0 and tracking()[0]==4880*256
        ref_state=lib.allocate()
        expected=apply(scene,instant,state=ref_state)[0]
        lib.release(ref_state)
        recovered=None
        for k in range(20):
            actual=apply(scene,p,reset=False)[0]
            if np.array_equal(actual,expected) and recovered is None:recovered=k+1
        assert recovered==3 and tracking()[0]==4500*256
        report['ffc_reacquire']['delayed_open_exact_recovery_frame']=recovered
        # A small cold object cannot replace an established background anchor.
        apply(np.full((200,200),4880,np.uint16),p)
        minor=np.full((200,200),4880,np.uint16);minor.flat[:1200]=4500
        for k in range(6):
            before=int(tracking()[0]);apply(minor,p,reset=False)
            assert abs(int(tracking()[0])-before)<=256
            assert lib.reacquiring(ws)==0
        # Check the90% boundary and reinitialize target/LUT even at Smooth99.
        p.smooth=99
        for fraction in (.89,.90):
            apply(np.full((200,200),4880,np.uint16),p)
            shifted=np.full((200,200),4880,np.uint16)
            shifted.flat[:round(shifted.size*fraction)]=4500
            for k in range(3):actual=apply(shifted,p,reset=False)[0]
            if fraction<.90:
                assert lib.reacquiring(ws)==0 and tracking()[0]>=4879*256
            else:
                ref_state=lib.allocate()
                expected=apply(shifted,instant,state=ref_state)[0]
                lib.release(ref_state)
                np.testing.assert_array_equal(actual,expected)
                assert tracking()[0]==4500*256 and tracking()[1]==92*256
                assert lib.reacquiring(ws)>0
        # A one-frame change must not count toward a later recovery sequence.
        apply(np.full((200,200),4880,np.uint16),p)
        apply(shifted,p,reset=False)
        apply(np.full((200,200),4880,np.uint16),p,reset=False)
        for k in range(2):
            apply(shifted,p,reset=False)
            assert lib.reacquiring(ws)==0
        apply(shifted,p,reset=False)
        assert lib.reacquiring(ws)>0
        print('PASS: delayed open recovers on third confirmed frame; 3% cold object retains rate limit')
        # R4 checks use the complete production RAW mapper, not a copied helper.
        def populations(*groups):
            return np.concatenate([np.full(count,dn,np.uint16) for dn,count in groups]).reshape(100,100)
        first=populations((3800,1000),(4200,8000),(4800,1000))
        second=populations((3800,8000),(4200,1000),(4800,1000))
        p=params();p.gain=5;p.smooth=0
        apply(first,p);assert tracking()[0]==4200*256
        reused=apply(second,p,reset=False)[0]
        assert tracking()[0]==3800*256
        np.testing.assert_array_equal(reused,apply(second,p)[0])
        p.smooth=50;apply(first,p)
        for _ in range(10):
            apply(second,p,reset=False)
            assert tracking()[0]==4200*256
        cold_object=populations((3800,1000),(4200,9000))
        apply(np.full((100,100),4200,np.uint16),p)
        for _ in range(10):
            apply(cold_object,p,reset=False)
            assert tracking()[0]==4200*256
        p.smooth=0;apply(cold_object,p)
        assert tracking()[0]==3800*256
        p.smooth=50;apply(np.full((100,100),4200,np.uint16),p)
        hot_majority=populations((4200,100),(4800,9900))
        for _ in range(10):
            apply(hot_majority,p,reset=False)
            assert tracking()[0]==4200*256
        lib.agc_weighted_begin_reacquire(ws)
        for _ in range(4):apply(first,p,reset=False)
        assert lib.reacquiring(ws)>0 and tracking()[0]==4200*256
        apply(second,p,reset=False)
        assert tracking()[0]==3800*256
        fractional=populations((3800,1000),(4200,4000),(4201,4000),(4800,1000))
        tests=[fractional,populations((0,1000),(8000,8000),(16383,1000)),
            populations((4000,5000),(4013,1000),(4026,4000)),
            populations((3800,29),(4200,1000),(4800,8971)),
            populations((3800,30),(4200,1000),(4800,8970))]
        p.smooth=0
        for sample in tests:
            actual,table=apply(sample,p)
            expected,_=reference(sample,p,toolkit)
            assert np.max(np.abs(table.astype(int)-expected.astype(int)))<=1
            assert tracking()[0]==round(reference_center(sample)[0]*256)
        apply(fractional,p);assert tracking()[0]==4200.5*256
        report['multigroup_r4']=dict(smooth0_history_output_max_difference_dn=0,
            first_three_group_center_dn=4200,second_three_group_center_dn=3800,
            two_group_history_background_dn=4200,two_group_without_history_dn=3800,
            hot_majority_fraction=.99,hot_majority_background_center_dn=4200,
            acquisition_ignores_history=True,fractional_group_median_dn=4200.5,
            supported_group_fraction=.003,minimum_separation_dn=70)
        print('PASS: R4 Smooth0 independence, 2/3-group history, reacquire, fractional median and sparse-group boundaries')
        print('PASS: FFC transition -> first valid scene exact LUT, bounded warmup and normal anti-jump resume')
        if args.before_dir:
            old=library(args.before_dir.resolve(),'before');old_ws=old.allocate()
            for a in cases:
                p=params();p.balanced=0;p.smooth=90
                for k in range(4):
                    current=apply(a,p,reset=k==0)[0]
                    before=apply(a,p,reset=k==0,api=old,state=old_ws)[0]
                    np.testing.assert_array_equal(current,before)
            # Temporal stress uses actual reported settings; these are synthetic
            # input RAWs, not a reconstruction of the decoded UVC recording.
            p=params();p.clip=80;p.bright=2;p.percentile=800;p.gain=5;p.smooth=50
            n=30000;rng_temporal=np.random.default_rng(172843)
            cool=np.resize(np.arange(4490,4511,dtype=np.uint16),n)
            warm=np.clip(np.rint(rng_temporal.normal(4780,80,n)),4511,5100).astype(np.uint16)
            fractions=np.r_[np.zeros(12),np.linspace(.02,.9,96),np.full(12,.9),np.linspace(.9,.02,96),np.zeros(12)]
            trajectory=[];previous_track=None
            for k,fraction in enumerate(fractions):
                count=int(round(n*fraction));a=cool.copy();a[:count]=warm[:count]
                # A sparse continuous-temperature bridge removes the easy gap,
                # so this sequence crosses Otsu's confidence boundary.
                bridge=np.arange(4511,5101,dtype=np.uint16)
                a[:min(count,len(bridge))]=bridge[:min(count,len(bridge))]
                a=a.reshape(150,200);background=np.arange(n).reshape(a.shape)>=count
                now,_=apply(a,p,reset=k==0)
                prior,_=apply(a,p,reset=k==0,api=old,state=old_ws)
                tr=tracking()
                if previous_track is not None:
                    assert abs(int(tr[0])-int(previous_track[0]))<=256
                    assert abs(int(tr[1])-int(previous_track[1]))<=128
                previous_track=tr.copy()
                trajectory.append(dict(frame=k,hot_area=float(fraction),
                    before_background_gray=float(prior[background].mean()*255/MAX),
                    after_background_gray=float(now[background].mean()*255/MAX),
                    tracked_center_dn=float(tr[0]/256),tracked_target_gray=float(tr[1]/256)))
            before_jump=float(np.abs(np.diff([r['before_background_gray'] for r in trajectory])).max())
            after_jump=float(np.abs(np.diff([r['after_background_gray'] for r in trajectory])).max())
            assert before_jump>5, before_jump
            assert after_jump<2, after_jump
            report['temporal_stress']=dict(scope='Synthetic changing-area RAW sequence; user settings Clip8/BW2/P80/Gain0.5/Smooth50',
                before_max_background_step_gray=before_jump,after_max_background_step_gray=after_jump,frames=trajectory)
            print(f'PASS: synthetic area transition, maximum background step {before_jump:.3f} -> {after_jump:.3f} Gray')
            old.release(old_ws)
            report['off_before_after_exact_cases']=len(cases)*4
            print('PASS: switch Off is bit-exact to the saved pre-change C implementation')
        if args.raw_dir:
            for path in sorted(args.raw_dir.glob('*.raw')):
                assert path.stat().st_size == 640*480*2
                a=np.fromfile(path,dtype='<u2').reshape(480,640)
                p=params();out,table=apply(a,p)
                expected,_=reference(a,p,toolkit)
                assert np.max(np.abs(table.astype(int)-expected.astype(int)))<=1
                toolkit_out,_=toolkit(a,MAX,clip_limit=2,redistribute=True,bright_weight=1,
                    bright_percentile=70,gain_limit=2,balanced_background=True)
                gray=out.astype(float)*255/MAX;error=np.abs(gray-toolkit_out)
                assert error.max() <= 1.0, (path.name, float(error.max()))
                # Separate Gray8 quantization from native mapping agreement.
                item=dict(source=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    toolkit_equivalent_gray_mae=float(error.mean()),toolkit_equivalent_gray_max=float(error.max()),
                    native_reference_max_dn=int(np.max(np.abs(table.astype(int)-expected.astype(int)))),
                    background_mean_gray=float(gray[20:120,20:220].mean()))
                report['real_raws'].append(item)
                if args.output_dir:
                    from PIL import Image
                    args.output_dir.mkdir(parents=True,exist_ok=True)
                    out.astype('<u2').tofile(args.output_dir/(path.stem+'_SoC_balanced_RAW14.raw'))
                    Image.fromarray(np.floor(gray+.5).astype(np.uint8)).save(args.output_dir/(path.stem+'_SoC_balanced.png'))
            print(json.dumps(report['real_raws'],ensure_ascii=True,indent=2))
        report['workspace_bytes']=lib.size();assert lib.size()<=230000
        lib.release(ws)
        if os.name=='nt':
            import _ctypes
            for loaded_lib in loaded:_ctypes.FreeLibrary(loaded_lib._handle)
    if dll_dir:dll_dir.close()
    if args.output_dir:
        args.output_dir.mkdir(parents=True,exist_ok=True)
        (args.output_dir/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')


if __name__=='__main__':main()
