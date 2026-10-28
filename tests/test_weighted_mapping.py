"""Compare firmware Weighted Mapping with the current Toolkit manual Tone Mapping.

python tests/test_weighted_mapping.py --toolkit <Thermal_Image_Toolkit directory>
Uses a host compiler and numpy. No camera, SDK library or generated repo artifacts.
"""
import argparse
import ctypes as ct
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('--toolkit', required=True, type=Path)
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
sys.path.insert(0, str(args.toolkit))
from thermal_toolkit.algorithms.histogram import histogram_weighted_mapping_native_u8
from thermal_toolkit.algorithms.weighted_aci import configure_weighted_aci

configure_weighted_aci({'enabled': False})
root = Path(__file__).resolve().parents[1]
compiler = shutil.which(args.cc)
if not compiler:
    raise SystemExit(f'Compiler not found: {args.cc}')
env = os.environ.copy()
env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
if os.name == 'nt':
    os.add_dll_directory(str(Path(compiler).parent))


class Params(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in (
        'clip', 'redistribute', 'bright', 'percentile', 'gain', 'smooth')]


with tempfile.TemporaryDirectory(prefix='tiffany_weighted_') as tmp:
    work = Path(tmp)
    wrapper = work / 'wrapper.c'
    wrapper.write_text('''#include <stdlib.h>
#include <string.h>
#include "agc_weighted.h"
#include "agc_mode.h"
AGC_MODE agc_mode_get(void) { return AGC_MODE_WEIGHTED_CPU; }
void *test_workspace(void) { return calloc(1, sizeof(AGC_WEIGHTED_WORKSPACE)); }
void test_free(void *p) { free(p); }
void test_lut(AGC_WEIGHTED_WORKSPACE *p, void *out) { memcpy(out, p->lut, sizeof(p->lut)); }
size_t test_workspace_size(void) { return sizeof(AGC_WEIGHTED_WORKSPACE); }
''')
    library = work / ('weighted.dll' if os.name == 'nt' else 'weighted.so')
    subprocess.run([compiler, '-shared', '-fPIC', '-std=gnu99', '-O2', '-Wall', '-Wextra', '-Werror',
        '-I', str(root / 'Enhancement/AGC'), str(wrapper),
        str(root / 'Enhancement/AGC/agc_weighted.c'), '-pthread', '-o', str(library)], check=True, env=env)
    lib = ct.CDLL(str(library))
    lib.test_workspace.restype = ct.c_void_p
    lib.test_free.argtypes = [ct.c_void_p]
    lib.test_lut.argtypes = [ct.c_void_p, ct.c_void_p]
    lib.test_workspace_size.restype = ct.c_size_t
    lib.agc_weighted_state_reset.argtypes = [ct.c_void_p]
    lib.agc_weighted_apply_u14.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_void_p, ct.c_size_t,
        ct.c_uint32, ct.c_uint32, ct.c_void_p, ct.POINTER(Params)]
    ws = lib.test_workspace()
    assert ws

    def apply(image, params, *, reset=True, inplace=False):
        if reset:
            lib.agc_weighted_state_reset(ws)
        source = np.ascontiguousarray(image, dtype=np.uint16)
        original = source.copy()
        output = source if inplace else np.zeros_like(source)
        h, w = source.shape
        assert lib.agc_weighted_apply_u14(source.ctypes.data, source.strides[0],
            output.ctypes.data, output.strides[0], w, h, ws, ct.byref(params)) == 0
        lut = np.zeros(16384, dtype=np.uint8)
        lib.test_lut(ws, lut.ctypes.data)
        if not inplace:
            np.testing.assert_array_equal(source, original)
        return output.copy(), lut

    rng = np.random.default_rng(20261028)
    cases = [np.zeros((8, 12), dtype=np.uint16), np.full((9, 13), 10000, dtype=np.uint16),
        np.array([0, 10, 16384, 65535], dtype=np.uint16).reshape(2, 2),
        np.array([0, 0, 100, 100, 10000, 16383], dtype=np.uint16).reshape(2, 3),
        np.arange(16384, dtype=np.uint16).reshape(128, 128),
        np.array([100] * 97 + [101, 200, 10000], dtype=np.uint16).reshape(10, 10)]
    for _ in range(35):
        levels = rng.choice(16384, int(rng.integers(2, 80)), replace=False).astype(np.uint16)
        cases.append(rng.choice(levels, (int(rng.integers(5, 35)), 27)))
    for _ in range(10):
        cases.append(rng.integers(10000, 10045, size=(32, 37), dtype=np.uint16))
    checks = 0
    try:
        for image in cases:
            for _ in range(12):
                p = Params(int(rng.choice([0, 1, 5, 10, 20, 100, 1000])), int(rng.integers(2)),
                    int(rng.choice([100, 105, 110, 120, 150, 200])),
                    int(rng.choice([0, 1, 500, 800, 999, 1000])),
                    int(rng.choice([0, 1, 5, 10, 20, 40])), 0)
                result, lut = apply(image, p)
                details, lut_data = {}, {}
                expected_u8, _ = histogram_weighted_mapping_native_u8(image, 16383,
                    clip_limit=p.clip / 10, redistribute=bool(p.redistribute), bright_weight=p.bright / 100,
                    bright_percentile=p.percentile / 10, gain_limit=p.gain / 10,
                    interval_data=details, lut_data=lut_data)
                np.testing.assert_array_equal(lut, lut_data['lut'], err_msg=f'params={list(bytes(p))}')
                expected = ((expected_u8.astype(np.uint32) * 16383 + 127) // 255).astype(np.uint16)
                np.testing.assert_array_equal(result, expected)
                inplace, _ = apply(image, p, inplace=True)
                np.testing.assert_array_equal(inplace, expected)
                checks += 1
        print(f'PASS: {checks} full LUT/output/in-place matches against Toolkit (manual Clip, populated-bin redistribution)')
        padded = np.full((11, 23), 65535, dtype=np.uint16)
        padded[:, :17] = rng.integers(10000, 10100, size=(11, 17), dtype=np.uint16)
        out = np.full_like(padded, 54321)
        p = Params(20, 1, 120, 800, 20, 0)
        lib.agc_weighted_state_reset(ws)
        assert lib.agc_weighted_apply_u14(padded.ctypes.data, padded.strides[0], out.ctypes.data,
            out.strides[0], 17, 11, ws, ct.byref(p)) == 0
        expected, _ = apply(padded[:, :17], p)
        np.testing.assert_array_equal(out[:, :17], expected)
        assert np.all(out[:, 17:] == 54321)
        print('PASS: stride padding preserved and RAW input unchanged')
        # New redistribution never populates empty bins: full LUT equality above
        # covers gaps both inside and outside the observed range and discarded excess.
        first, second = cases[-1], np.flip(cases[-1], axis=1).copy() + 17
        p = Params(20, 1, 120, 800, 0, 0)
        _, a = apply(first, p)
        _, b = apply(second, p)
        p.smooth = 90
        apply(first, p)
        _, mixed = apply(second, p, reset=False)
        tenths = (b.astype(np.uint32) * 10 * 10 + a.astype(np.uint32) * 10 * 90 + 50) // 100
        np.testing.assert_array_equal(mixed, ((tenths + 5) // 10).astype(np.uint8))
        lib.agc_weighted_state_reset(ws)
        _, reset_lut = apply(second, p, reset=False)
        np.testing.assert_array_equal(reset_lut, b)
        print('PASS: LUT Smooth 90/10 and history reset retained')
        assert lib.test_workspace_size() < 181000
        print(f'PASS: workspace {lib.test_workspace_size()} bytes; no per-pixel RAW history allocation')
    finally:
        lib.test_free(ws)
        if os.name == 'nt':
            import _ctypes
            _ctypes.FreeLibrary(lib._handle)
