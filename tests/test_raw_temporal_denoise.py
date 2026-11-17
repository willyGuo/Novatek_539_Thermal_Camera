"""Host checks and optional byte parity with the saved offline RAW experiment.
No target CPU/FPS or real-motion quality proof.
"""
import argparse
import csv
import ctypes as ct
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import numpy as np

p = argparse.ArgumentParser()
p.add_argument('--cc', default='gcc')
p.add_argument('--raw-dir', type=Path)
p.add_argument('--reference-dir', type=Path)
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
cc = shutil.which(args.cc)
assert cc
env = os.environ.copy(); env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
dll_path = os.add_dll_directory(str(Path(cc).parent)) if os.name == 'nt' else None

class Params(ct.Structure):
    _fields_ = [('enable', ct.c_uint32), ('threshold', ct.c_uint32), ('shift', ct.c_uint32)]
class State(ct.Structure):
    _fields_ = [('history', ct.c_void_p), ('capacity', ct.c_size_t), ('width', ct.c_uint32),
                ('height', ct.c_uint32), ('time', ct.c_uint64), ('valid', ct.c_int), ('params', Params)]

with tempfile.TemporaryDirectory(prefix='raw_temporal_', ignore_cleanup_errors=True) as tmp:
    tmp = Path(tmp)
    common = [cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-D__LINUX', '-I', str(root)]
    source = str(root/'Enhancement/RAW/raw_temporal_denoise.c')
    exe = tmp/'unit.exe'
    subprocess.run(common + [str(root/'tests/test_raw_temporal_denoise.c'), source, '-pthread', '-o', str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)
    if args.raw_dir:
        assert args.reference_dir
        dll = tmp/'filter.dll'
        subprocess.run(common + ['-shared', '-fPIC', source, '-pthread', '-o', str(dll)], check=True, env=env)
        lib = ct.CDLL(str(dll))
        lib.raw_temporal_denoise_process.argtypes = [ct.POINTER(State), ct.c_void_p, ct.c_size_t,
            ct.c_uint32, ct.c_uint32, ct.c_uint64, ct.POINTER(Params)]
        lib.raw_temporal_denoise_destroy.argtypes = [ct.POINTER(State)]
        with (args.raw_dir/'summary.csv').open(encoding='utf-8-sig', newline='') as f:
            metadata = list(csv.DictReader(f))
        for name, threshold, shift in [('light_T8_prev50', 8, 1), ('medium_T12_prev75', 12, 2), ('strong_T16_prev87p5', 16, 3)]:
            state = State(); params = Params(1, threshold, shift)
            for item in metadata:
                file = item['raw_file']
                a = np.fromfile(args.raw_dir/file, dtype='<u2').reshape(480, 640)
                timestamp = int(item['capture_unix_ms']) * 1000
                assert lib.raw_temporal_denoise_process(ct.byref(state), a.ctypes.data, 1280, 640, 480,
                    timestamp, ct.byref(params)) > 0
                expected = np.fromfile(args.reference_dir/name/file, dtype='<u2').reshape(a.shape)
                np.testing.assert_array_equal(a, expected, err_msg=file)
            lib.raw_temporal_denoise_destroy(ct.byref(state))
            print(f'PASS: {name}, {len(metadata)} production-C frames exactly match offline reference')
        if os.name == 'nt':
            import _ctypes
            _ctypes.FreeLibrary(lib._handle)
if dll_path: dll_path.close()
