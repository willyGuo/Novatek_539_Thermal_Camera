"""Half-resolution float-base DDE: numerical reference, timing and image artifacts.

Host only. Images are already-rendered Y8 outputs, not pre-DDE board frames.
"""
from contextlib import ExitStack
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
from PIL import Image, ImageDraw, ImageFont
from test_dde_1106 import reference_base

ROOT = Path(__file__).resolve().parents[1]


def half_reference(image, sigma_s, sigma_r, gain, passes):
    h, w = image.shape
    padded = np.pad(image, ((0, h % 2), (0, w % 2)), mode='edge').astype(np.uint32)
    small = ((padded.reshape((h+1)//2, 2, (w+1)//2, 2).sum(axis=(1, 3)) + 2) // 4).astype(np.uint8)
    base = reference_base(small, sigma_s/2, sigma_r, passes)
    # Interpolate from true box centers, including singleton last rows/columns.
    xs = np.array([(x*2 + min(x*2+1, w-1))/2 for x in range(small.shape[1])])
    ys = np.array([(y*2 + min(y*2+1, h-1))/2 for y in range(small.shape[0])])
    rows = np.array([np.interp(np.arange(w), xs, row) for row in base])
    large = np.array([np.interp(np.arange(h), ys, rows[:, x]) for x in range(w)]).T
    return np.floor(np.clip(large + (image.astype(float)-large)*gain, 0, 255)+.5).astype(np.uint8)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--image', action='append', type=Path, default=[])
    args = parser.parse_args()
    cc = shutil.which('gcc'); assert cc
    env = os.environ.copy()
    env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
    report = {'scope': 'Host core only, not board FPS; offline PNGs are rendered outputs, not pre-DDE Y8',
              'host_machine': platform.machine(),
              'compiler': subprocess.check_output([cc, '--version'], env=env, text=True).splitlines()[0],
              'build': 'GCC O3, SIMD4 explicitly forced on host; no ARM execution proof',
              'settings': {'sigma_s_full_pixels': 21, 'sigma_r_y8': 51, 'gain': 2, 'passes': 2},
              'core_sha256': hashlib.sha256((ROOT/'Enhancement/DDE/dde_dt_opt.c').read_bytes()).hexdigest()}
    with tempfile.TemporaryDirectory(prefix='dde_half_') as temporary, ExitStack() as stack:
        temporary = Path(temporary)
        if os.name == 'nt': stack.enter_context(os.add_dll_directory(str(Path(cc).parent)))
        def build(simd):
            path = temporary / (f'core{simd}.dll' if os.name == 'nt' else f'core{simd}.so')
            subprocess.run([cc, '-std=gnu99', '-O3', '-Wall', '-Wextra', '-Werror',
                f'-DDDE_SIMD4={simd}', '-shared', '-fPIC', str(ROOT/'Enhancement/DDE/dde_dt_opt.c'),
                '-lm', '-o', str(path)], check=True, env=env)
            lib = ct.CDLL(str(path))
            if os.name == 'nt':
                import _ctypes
                stack.callback(_ctypes.FreeLibrary, lib._handle)
            lib.dde_dt_scaled_workbuf_size.argtypes = [ct.c_uint32]*3
            lib.dde_dt_scaled_workbuf_size.restype = ct.c_uint32
            lib.DDE_DT_Optimized_Scaled.argtypes = [ct.c_void_p, ct.c_uint32, ct.c_uint32, ct.c_uint32,
                ct.c_float, ct.c_float, ct.c_float, ct.c_int, ct.c_uint8, ct.c_uint8, ct.c_uint32, ct.c_void_p]
            lib.DDE_DT_Optimized.argtypes = lib.DDE_DT_Optimized_Scaled.argtypes[:10]+[ct.c_void_p]
            return lib
        scalar, simd = build(0), build(1)
        def apply(lib, image, scale=2, passes=2, gain=2, sigma=(21, 51)):
            h, w = image.shape
            padded = np.full((h+2, w+7), 173, np.uint8)
            padded[1:-1, :w] = image
            size = lib.dde_dt_scaled_workbuf_size(w, h, scale); assert size
            work = (ct.c_uint8*(size+128))()
            ct.memset(work, 0xA5, size+128)
            assert lib.DDE_DT_Optimized_Scaled(padded.ctypes.data+padded.shape[1], w, h,
                padded.shape[1], *sigma, gain, passes, 255, 0, scale, ct.addressof(work)+64) == 0
            assert bytes(work[:64]) == bytes(work[-64:]) == b'\xa5'*64
            assert np.all(padded[0] == 173) and np.all(padded[-1] == 173)
            assert np.all(padded[1:-1, w:] == 173)
            return padded[1:-1, :w].copy()
        rng = np.random.default_rng(110613)
        count, max_error = 0, 0
        for shape in [(1, 1), (1, 19), (19, 1), (7, 15), (16, 32), (33, 65)]:
            for constant in [False, True]:
                image = np.full(shape, 127, np.uint8) if constant else rng.integers(0, 256, shape, dtype=np.uint8)
                for passes in [1, 2, 3, 8]:
                    for gain in [0, 1, 2, 10]:
                        actual = apply(simd, image, passes=passes, gain=gain)
                        assert np.array_equal(actual, apply(scalar, image, passes=passes, gain=gain))
                        expected = half_reference(image, 21, 51, gain, passes)
                        error = int(np.abs(actual.astype(int)-expected.astype(int)).max())
                        assert error <= 1, (shape, passes, gain, error)
                        max_error = max(max_error, error)
                        if gain == 1: assert np.array_equal(actual, image)
                        count += 1
        for shape in [(480, 640), (512, 640), (481, 641)]:
            image = rng.integers(0, 256, shape, dtype=np.uint8)
            assert np.array_equal(apply(simd, image), apply(scalar, image))
            assert np.abs(apply(simd, image).astype(int)-half_reference(image, 21, 51, 2, 2).astype(int)).max() <= 1
            full = image.copy()
            work = ct.create_string_buffer(simd.dde_dt_scaled_workbuf_size(shape[1], shape[0], 1))
            assert simd.DDE_DT_Optimized(full.ctypes.data, shape[1], shape[0], shape[1],
                21, 51, 2, 2, 255, 0, ct.addressof(work)) == 0
            assert np.array_equal(full, apply(simd, image, scale=1))
            count += 1
        image = np.full((8, 8), 77, np.uint8)
        work = ct.create_string_buffer(simd.dde_dt_scaled_workbuf_size(8, 8, 2))
        valid = [image.ctypes.data, 8, 8, 8, 21, 51, 2, 2, 255, 0, 2, ct.addressof(work)]
        for index, value in [(0, None), (1, 0), (3, 7), (4, float('nan')), (5, 0),
                             (6, -1), (7, 0), (8, 0), (10, 0), (10, 3), (11, None)]:
            invalid = valid.copy();invalid[index] = value
            assert simd.DDE_DT_Optimized_Scaled(*invalid) < 0
            assert np.all(image == 77)
        report['numeric_cases'] = count
        report['max_error_vs_independent_reference_gray'] = max_error
        print(f'PASS: {count} half-base cases, independent reference <=1 Gray; SIMD/scalar exact; full-mode unchanged; guards/stride/errors.')
        report['host_benchmarks'] = []
        for height in [480, 512]:
            original = rng.integers(0, 256, (height, 640), dtype=np.uint8)
            image = original.copy()
            work = ct.create_string_buffer(max(simd.dde_dt_scaled_workbuf_size(640, height, n) for n in [1, 2]))
            samples = {name: [] for name in ['full_1_pass', 'full_2_pass', 'half_2_pass']}
            variants = [('full_1_pass', 1, 1), ('full_2_pass', 1, 2), ('half_2_pass', 2, 2)]
            for k in range(35):
                for name, scale, passes in (variants if k%2 == 0 else variants[::-1]):
                    np.copyto(image, original)
                    start = time.perf_counter_ns()
                    assert simd.DDE_DT_Optimized_Scaled(image.ctypes.data, 640, height, 640,
                        21, 51, 2, passes, 255, 0, scale, ct.addressof(work)) == 0
                    elapsed = (time.perf_counter_ns()-start)/1e6
                    if k >= 4: samples[name].append(elapsed)
            result = {'width': 640, 'height': height,
                      'host_median_ms': {name: float(np.median(values)) for name, values in samples.items()},
                      'full_workspace_bytes': simd.dde_dt_scaled_workbuf_size(640, height, 1),
                      'half_workspace_bytes': simd.dde_dt_scaled_workbuf_size(640, height, 2)}
            report['host_benchmarks'].append(result)
            print('HOST ONLY:', result)
        report['images'] = []
        if args.output_dir:
            args.output_dir.mkdir(parents=True, exist_ok=True)
            font_path = 'C:/Windows/Fonts/msjh.ttc'
            font, small_font = ImageFont.truetype(font_path, 24), ImageFont.truetype(font_path, 19)
            for index, source in enumerate(args.image):
                before = hashlib.sha256(source.read_bytes()).hexdigest()
                original = Image.open(source).convert('L')
                roi = None
                if original.width == 1280 and original.height == 480:
                    roi = [0, 35, 640, 480]  # Existing comparison: PEGA panel, exclude title.
                    original = original.crop(roi)
                image = np.asarray(original)
                outputs = [apply(simd, image, scale=1, passes=1),
                           apply(simd, image, scale=1, passes=2), apply(simd, image, scale=2, passes=2)]
                prefix = f'{index+1:02d}_{source.stem}'
                original.save(args.output_dir/(prefix+'_input_y8.png'))
                for name, array in zip(['full_1_pass', 'full_2_pass', 'half_2_pass'], outputs):
                    Image.fromarray(array).save(args.output_dir/(prefix+'_'+name+'.png'))
                difference = outputs[2].astype(float)-outputs[1].astype(float)
                h, w = image.shape
                canvas = Image.new('RGB', (w*3+64, h+140), '#151b25')
                draw = ImageDraw.Draw(canvas)
                draw.text((16, 12), source.stem+' | same Y8 input, S21/R51/gain2', font=font, fill='white')
                for n, (name, array) in enumerate(zip(['Full base / 1 pass', 'Full base / 2 passes', 'Half base / 2 passes'], outputs)):
                    x = 16+n*(w+16)
                    draw.text((x, 52), name, font=font, fill='white')
                    canvas.paste(Image.fromarray(array).convert('RGB'), (x, 90))
                draw.text((16, h+106), 'Fixed 0..255 display. Rendered PNG replay; not live pre-DDE input or board FPS proof.', font=small_font, fill='#becadb')
                target = args.output_dir/(prefix+'_comparison.png');canvas.save(target)
                with Image.open(target) as verified: verified.verify()
                worst_y, worst_x = np.unravel_index(np.abs(difference).argmax(), difference.shape)
                crop_w, crop_h = min(128, w), min(128, h)
                left = min(max(int(worst_x)-crop_w//2, 0), w-crop_w)
                top = min(max(int(worst_y)-crop_h//2, 0), h-crop_h)
                roi_detail = [left, top, left+crop_w, top+crop_h]
                detail = Image.new('RGB', (800, 960), '#151b25')
                draw = ImageDraw.Draw(detail)
                draw.text((16, 10), source.stem+' | ROI around maximum difference', font=font, fill='white')
                draw.text((16, 43), f'ROI {roi_detail}, 3x nearest-neighbor', font=small_font, fill='#becadb')
                delta = np.floor(np.clip(128+difference*4, 0, 255)+.5).astype(np.uint8)
                for n, (name, array) in enumerate(zip(
                    ['Full base / 1 pass', 'Full base / 2 passes', 'Half base / 2 passes', 'Signed half-full difference x4'],
                    outputs+[delta])):
                    x, y = 16+(n%2)*392, 86+(n//2)*424
                    draw.text((x, y), name, font=small_font, fill='white')
                    patch = Image.fromarray(array).crop(roi_detail).resize((crop_w*3, crop_h*3), Image.Resampling.NEAREST)
                    detail.paste(patch.convert('RGB'), (x, y+30))
                detail_path = args.output_dir/(prefix+'_detail.png');detail.save(detail_path)
                with Image.open(detail_path) as verified: verified.verify()
                assert hashlib.sha256(source.read_bytes()).hexdigest() == before
                report['images'].append({'source': str(source), 'source_sha256': before, 'roi': roi,
                    'comparison': str(target), 'width': w, 'height': h,
                    'detail': str(detail_path), 'detail_roi': roi_detail,
                    'half_vs_full_2_pass_mae_gray': float(np.abs(difference).mean()),
                    'half_vs_full_2_pass_max_difference_gray': float(np.abs(difference).max()),
                    'half_vs_full_2_pass_changed_percent': float(np.count_nonzero(difference)*100/difference.size)})
            destination = args.output_dir/'analysis.json'
            destination.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
            print('Report:', destination)


if __name__ == '__main__': main()
