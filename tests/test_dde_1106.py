"""Host checks for DDE_1106 Y8 port; does not prove SDK/board behavior.

Optional --reference points to the colleague's unchanged DDE_1106 directory.
"""
import argparse
import ctypes as ct
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def reference_base(image, sigma_s, sigma_r, iterations):
    """Direct full-plane directional recurrence, without tiles or in-place guide."""
    guide = image.astype(np.float32)
    base = guide.copy()
    count = (iterations + 1) // 2
    for iteration in range(iterations):
        sigma = sigma_s * np.sqrt(3) * 2 ** (count - iteration // 2 - 1)
        sigma /= np.sqrt(4 ** count - 1)
        horizontal = np.exp(-np.sqrt(2) / sigma * np.sqrt(
            1 + (sigma / sigma_r * np.diff(guide, axis=1)) ** 2))
        vertical = np.exp(-np.sqrt(2) / sigma * np.sqrt(
            1 + (sigma / sigma_r * np.diff(guide, axis=0)) ** 2))
        if iteration % 2 == 0:
            for x in range(1, image.shape[1]):
                a = horizontal[:, x - 1]
                base[:, x] = (1 - a) * base[:, x] + a * base[:, x - 1]
            for y in range(1, image.shape[0]):
                a = vertical[y - 1]
                base[y] = (1 - a) * base[y] + a * base[y - 1]
        else:
            for x in range(image.shape[1] - 2, -1, -1):
                a = horizontal[:, x]
                base[:, x] = (1 - a) * base[:, x] + a * base[:, x + 1]
            for y in range(image.shape[0] - 2, -1, -1):
                a = vertical[y]
                base[y] = (1 - a) * base[y] + a * base[y + 1]
    return base


def reference(image, sigma_s, sigma_r, gain, iterations):
    base = reference_base(image, sigma_s, sigma_r, iterations)
    return np.floor(np.clip(base + (image.astype(np.float32) - base) * gain, 0, 255) + .5).astype(np.uint8)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--benchmark', action='store_true', help='host core timing only, not board FPS')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    cc = shutil.which('gcc')
    assert cc, 'gcc is required'
    env = os.environ.copy()
    env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
    dll_directory = os.add_dll_directory(str(Path(cc).parent)) if os.name == 'nt' else None
    report = {'scope': 'Host only; does not prove Novatek SDK linking, board FPS or image quality',
              'host_machine': platform.machine(),
              'compiler': subprocess.check_output([cc, '--version'], env=env, text=True).splitlines()[0],
              'core_sha256': hashlib.sha256((ROOT/'Enhancement/DDE/dde_dt_opt.c').read_bytes()).hexdigest(),
              'build': 'Production and original-formula baseline: O3; unchanged colleague reference: O2',
              'simd_test_scope': 'SIMD4 forced on host via GNU vectors; not ARM/NEON execution proof',
              'benchmark_parameters': {'sigma_s': 21, 'sigma_r': 51, 'gain': 2,
                                       'warmup_rounds': 4, 'measured_rounds': 31,
                                       'excludes': 'image reset copy, HDAL mapping/cache, encoder/UVC'}}
    with tempfile.TemporaryDirectory(prefix='dde_1106_', ignore_cleanup_errors=True) as temporary:
        temporary = Path(temporary)

        def build(name, source, flags=()):
            output = temporary / (name + ('.dll' if os.name == 'nt' else '.so'))
            # The untouched colleague source triggers a GCC O3 false-positive
            # maybe-uninitialized warning; keep its prior O2 reference build.
            optimization = '-O2' if name == 'colleague' else '-O3'
            subprocess.run([cc, '-std=gnu99', optimization, '-Wall', '-Wextra', '-Werror',
                            '-shared', '-fPIC', *flags, str(source), '-lm', '-o', str(output)],
                           check=True, env=env)
            lib = ct.CDLL(str(output))
            lib.dde_dt_get_workbuf_size.argtypes = [ct.c_uint32, ct.c_uint32]
            lib.dde_dt_get_workbuf_size.restype = ct.c_uint32
            lib.DDE_DT_Optimized.argtypes = [ct.c_void_p, ct.c_uint32, ct.c_uint32,
                ct.c_uint32, ct.c_float, ct.c_float, ct.c_float, ct.c_int,
                ct.c_uint16 if name == 'colleague' else ct.c_uint8,
                ct.c_uint16 if name == 'colleague' else ct.c_uint8, ct.c_void_p]
            return lib

        source = ROOT / 'Enhancement/DDE/dde_dt_opt.c'
        fused = build('fused', source, ['-DDDE_SIMD4=1'])
        scalar = build('scalar', source, ['-DDDE_SIMD4=0'])
        baseline = build('baseline', source, ['-DDDE_RECUR_LERP=0', '-DDDE_SIMD4=0'])
        unfused = build('unfused', source, ['-DDDE_STRIP_FUSE=0', '-DDDE_SIMD4=1'])
        for lib in [fused, scalar]:
            lib.dde_dt_backend_name.restype = ct.c_char_p
        assert b'SIMD4' in fused.dde_dt_backend_name()
        assert scalar.dde_dt_backend_name() == b'scalar'
        # Exercise optional branches too, including tiny-image histogram allocation.
        mapping = build('mapping', source, ['-DDDE_USING_MAPPING=1'])
        symmetric = build('symmetric', source, ['-DDDE_DIR_MODE=0'])
        colleague = build('colleague', args.reference / 'dde_dt_opt.c') if args.reference else None

        def run(lib, image, iterations, gain, dtype=np.uint8, sigma=(21, 51)):
            height, width = image.shape
            stride = width + 7
            padded = np.full((height + 2, stride), 173, dtype=dtype)
            padded[1:-1, :width] = image
            size = lib.dde_dt_get_workbuf_size(width, height)
            assert size
            work = (ct.c_uint8 * (size + 128))()
            ct.memset(work, 0xA5, size + 128)
            rc = lib.DDE_DT_Optimized(padded.ctypes.data + stride * padded.itemsize,
                width, height, stride, *sigma, gain, iterations, 255, 0,
                ct.addressof(work) + 64)
            assert rc == 0, rc
            assert bytes(work[:64]) == b'\xa5' * 64
            assert bytes(work[-64:]) == b'\xa5' * 64
            assert np.all(padded[0] == 173) and np.all(padded[-1] == 173)
            assert np.all(padded[1:-1, width:] == 173), 'row padding changed'
            return padded[1:-1, :width].copy()

        rng = np.random.default_rng(1106)
        cases = [rng.integers(0, 256, shape, dtype=np.uint8) for shape in
                 [(1, 1), (1, 19), (19, 1), (7, 15), (9, 16), (17, 31), (33, 65)]]
        cases += [np.full((19, 35), n, np.uint8) for n in [0, 127, 255]]
        cases += [np.tile(np.array([0, 255] * 17, np.uint8), (19, 1))]
        comparisons = 0
        differences = {'max_error_gray': 0, 'changed_pixels': 0, 'pixels_compared': 0}

        def compare(actual, original):
            error = np.abs(actual.astype(int) - original.astype(int))
            maximum = int(error.max())
            assert maximum <= 1, maximum
            differences['max_error_gray'] = max(differences['max_error_gray'], maximum)
            differences['changed_pixels'] += int(np.count_nonzero(error))
            differences['pixels_compared'] += int(error.size)
        for image in cases:
            for iterations in [1, 2, 3, 4, 8]:
                for gain in [0, 1, 2, 10]:
                    actual = run(fused, image, iterations, gain)
                    assert np.array_equal(actual, run(scalar, image, iterations, gain))
                    assert np.array_equal(actual, run(unfused, image, iterations, gain))
                    original = run(baseline, image, iterations, gain)
                    compare(actual, original)
                    expected = reference(image, 21, 51, gain, iterations)
                    assert np.max(np.abs(actual.astype(int) - expected.astype(int))) <= 1
                    if gain == 1:
                        assert np.array_equal(actual, image)
                    if colleague:
                        assert np.array_equal(original, run(colleague, image, iterations, gain, np.uint16))
                    comparisons += 1
            run(mapping, image, 3, 2)
            run(symmetric, image, 3, 2)
        for shape in [(480, 640), (512, 640)]:
            image = rng.integers(0, 256, shape, dtype=np.uint8)
            for iterations in [1, 2, 3, 8]:
                actual = run(fused, image, iterations, 2)
                assert np.array_equal(actual, run(scalar, image, iterations, 2))
                assert np.array_equal(actual, run(unfused, image, iterations, 2))
                original = run(baseline, image, iterations, 2)
                compare(actual, original)
                if colleague:
                    assert np.array_equal(original, run(colleague, image, iterations, 2, np.uint16))
                comparisons += 1
        # Two complete passes at the supported sigma/gain extremes and edges.
        edge = np.zeros((33, 65), np.uint8)
        edge[:, 32:] = 255
        low_contrast = rng.integers(124, 132, (33, 65), dtype=np.uint8)
        for image in [edge, low_contrast]:
            for sigma_s in [1, 21, 256]:
                for sigma_r in [1, 51, 255]:
                    for gain in [0, 1, 2, 10]:
                        actual = run(fused, image, 2, gain, sigma=(sigma_s, sigma_r))
                        assert np.array_equal(actual, run(scalar, image, 2, gain, sigma=(sigma_s, sigma_r)))
                        compare(actual, run(baseline, image, 2, gain, sigma=(sigma_s, sigma_r)))
                        expected = reference(image, sigma_s, sigma_r, gain, 2)
                        assert np.max(np.abs(actual.astype(int) - expected.astype(int))) <= 1
                        comparisons += 1
        assert fused.dde_dt_get_workbuf_size(0, 1) == 0
        assert fused.dde_dt_get_workbuf_size(4097, 1) == 0
        image = np.full((4, 8), 127, np.uint8)
        original = image.copy()
        size = fused.dde_dt_get_workbuf_size(8, 4)
        work = ct.create_string_buffer(size)
        valid = [image.ctypes.data, 8, 4, 8, 21, 51, 2, 3, 255, 0, ct.addressof(work)]
        for index, value in [(0, None), (1, 0), (1, 4097), (3, 7),
                             (4, float('nan')), (4, float('inf')), (5, 0),
                             (5, 256), (6, float('inf')), (6, -1), (6, 11),
                             (7, 0), (7, 9), (8, 0), (10, None)]:
            invalid = valid.copy()
            invalid[index] = value
            assert fused.DDE_DT_Optimized(*invalid) < 0
            assert np.array_equal(image, original)
        report['numeric_cases'] = comparisons
        report['simd_vs_scalar'] = 'exact output match in all numeric cases'
        report['optimized_vs_original'] = differences
        print(f'PASS: {comparisons} numeric cases; SIMD/scalar and fused/unfused parity; '
              'independent recurrence <=1 Y8 DN; padding/workspace guards; invalid inputs.')
        if colleague:
            print('PASS: original-formula baseline exactly matches colleague uint16 core on Y8 values/bounds.')
        print(f'LERP vs original: {differences}')
        for height in [480, 512]:
            print(f'Y8 workspace 640x{height}: {fused.dde_dt_get_workbuf_size(640, height)} bytes')
        if args.benchmark:
            report['host_core_benchmark'] = []
            for height in [480, 512]:
                width = 640
                original = rng.integers(0, 256, (height, width), dtype=np.uint8)
                target = original.copy()
                work = ct.create_string_buffer(fused.dde_dt_get_workbuf_size(width, height))
                # Warm up each variant; time only the core, reset image outside the timer.
                variants = [('original_1_pass', baseline, 1), ('original_2_pass', baseline, 2),
                            ('scalar_lerp_2_pass', scalar, 2), ('simd4_2_pass', fused, 2)]
                samples = {name: [] for name, _, _ in variants}
                for iteration in range(35):
                    order = variants if iteration % 2 == 0 else variants[::-1]
                    for name, lib, passes in order:
                        np.copyto(target, original)
                        start = time.perf_counter_ns()
                        rc = lib.DDE_DT_Optimized(target.ctypes.data, width, height, width,
                            21, 51, 2, passes, 255, 0, ct.addressof(work))
                        elapsed_ms = (time.perf_counter_ns() - start) / 1e6
                        assert rc == 0
                        if iteration >= 4:
                            samples[name].append(elapsed_ms)
                result = {'width': width, 'height': height,
                          'median_ms': {name: float(np.median(values)) for name, values in samples.items()}}
                result['two_pass_speedup_ratio'] = (result['median_ms']['original_2_pass'] /
                                                    result['median_ms']['simd4_2_pass'])
                result['simd_vs_previous_lerp_ratio'] = (result['median_ms']['scalar_lerp_2_pass'] /
                                                         result['median_ms']['simd4_2_pass'])
                report['host_core_benchmark'].append(result)
                print(f'HOST ONLY benchmark: {result}')
        if os.name == 'nt':
            import _ctypes
            for lib in [fused, scalar, baseline, unfused, mapping, symmetric, colleague]:
                if lib is not None:
                    _ctypes.FreeLibrary(lib._handle)
    if dll_directory:
        dll_directory.close()
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
