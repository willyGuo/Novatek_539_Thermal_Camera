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
from raw_denoise_sources import RAW_DENOISE_SOURCES

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
    _fields_ = [(name, ct.c_uint32) for name in (
        'enable', 'threshold', 'shift', 'spatial_enable', 'spatial_threshold',
        'dual_speed_enable', 'motion_threshold', 'motion_strong_threshold',
        'motion_hysteresis', 'motion_hold', 'spatial_mode')]
class State(ct.Structure):
    _fields_ = [('history', ct.c_void_p), ('capacity', ct.c_size_t), ('width', ct.c_uint32),
                ('height', ct.c_uint32), ('time', ct.c_uint64), ('valid', ct.c_int), ('params', Params),
                ('slow_history', ct.c_void_p), ('motion', ct.c_void_p), ('dual_capacity', ct.c_size_t),
                ('spatial_row', ct.c_void_p), ('row_capacity', ct.c_size_t)]


def check_noise(lib):
    metrics = {}
    rng = np.random.default_rng(539)
    frames = np.rint(10000 + rng.normal(0, 2.518, (512, 32, 48))).astype(np.uint16)
    input_sd = frames[160:, 1:-1, 1:-1].astype(np.float64).std()
    cases = [
        ('legacy50', Params(1, 16383, 1), np.sqrt(1 / 3), False),
        ('spatial', Params(1, 16383, 1, 1, 16383), np.sqrt(20 / 64), True),
        ('spatial_legacy50', Params(1, 16383, 1, 1, 16383), np.sqrt(20 / 192), False),
        ('dual_slow', Params(1, 12, 2, 0, 8, 1, 100, 200, 1, 3), np.sqrt(1 / 31), False),
        ('spatial_slow', Params(1, 12, 2, 1, 16383, 1, 100, 200, 1, 3),
         np.sqrt(20 / (64 * 31)), False),
        ('candidate_8_3_8', Params(1, 12, 2, 1, 8, 1, 3, 8, 1, 3), None, False),
        ('selective_8_3_8', Params(1, 12, 2, 1, 8, 1, 3, 8, 1, 3, 1), None, False),
    ]
    for name, params, expected_ratio, reset_each in cases:
        state = State()
        q8_samples, output_samples = [], []
        try:
            for index, frame in enumerate(frames):
                image = frame.copy()
                if reset_each:
                    lib.raw_temporal_denoise_reset(ct.byref(state))
                result = lib.raw_temporal_denoise_process(ct.byref(state), image.ctypes.data,
                    image.strides[0], image.shape[1], image.shape[0], (index + 1) * 33000, ct.byref(params))
                assert result == (2 if index == 0 or reset_each else 1)
                if index >= 160:
                    pointer = state.slow_history if params.dual_speed_enable else state.history
                    history = np.ctypeslib.as_array(ct.cast(pointer, ct.POINTER(ct.c_uint32)),
                        shape=(image.size,)).reshape(image.shape)
                    q8_samples.append(history[1:-1, 1:-1].astype(np.float64) / 256)
                    output_samples.append(image[1:-1, 1:-1].copy())
            internal_sd = np.std(q8_samples)
            output_sd = np.std(output_samples)
            metrics[name] = output_sd
            if expected_ratio is not None:
                np.testing.assert_allclose(internal_sd / input_sd, expected_ratio, rtol=0.05)
            else:
                assert output_sd < input_sd * 0.4
            print(f'PASS: synthetic {name}: input SD={input_sd:.4f}, '
                  f'history SD={internal_sd:.4f}, UINT16 output SD={output_sd:.4f} DN')
        finally:
            lib.raw_temporal_denoise_destroy(ct.byref(state))
    assert metrics['selective_8_3_8'] < metrics['candidate_8_3_8'] * 0.8


with tempfile.TemporaryDirectory(prefix='raw_temporal_', ignore_cleanup_errors=True) as tmp:
    tmp = Path(tmp)
    common = [cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-D__LINUX', '-I', str(root)]
    exe = tmp/'unit.exe'
    unit_object = tmp/'filter_unit.o'
    subprocess.run(common + ['-Dmalloc=raw_test_malloc', '-Dfree=raw_test_free', '-c', RAW_DENOISE_SOURCES[0],
        '-o', str(unit_object)], check=True, env=env)
    subprocess.run(common + ['-DRAW_TEMPORAL_TEST_ALLOCATOR', str(root/'tests/test_raw_temporal_denoise.c'),
        str(unit_object), *RAW_DENOISE_SOURCES[1:], '-pthread', '-o', str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)
    dll = tmp/'filter.dll'
    subprocess.run(common + ['-shared', '-fPIC', *RAW_DENOISE_SOURCES, '-pthread', '-o', str(dll)], check=True, env=env)
    lib = ct.CDLL(str(dll))
    lib.raw_temporal_denoise_process.argtypes = [ct.POINTER(State), ct.c_void_p, ct.c_size_t,
        ct.c_uint32, ct.c_uint32, ct.c_uint64, ct.POINTER(Params)]
    lib.raw_temporal_denoise_destroy.argtypes = [ct.POINTER(State)]
    lib.raw_temporal_denoise_reset.argtypes = [ct.POINTER(State)]
    check_noise(lib)
    if args.raw_dir:
        assert args.reference_dir
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
