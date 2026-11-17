"""Analyze captured probe RAWs. No rescaling is allowed in the bypass verdict."""
import argparse
from array import array
import hashlib
import json
from math import gcd, sqrt
from pathlib import Path
import sys

WIDTH, HEIGHT = 640, 480

def read_raw(path):
    raw = path.read_bytes()
    if len(raw) != WIDTH * HEIGHT * 2:
        raise ValueError(f'{path}: expected 614400 bytes, got {len(raw)}')
    values = array('H')
    values.frombytes(raw)
    if sys.byteorder != 'little':
        values.byteswap()
    return values, hashlib.sha256(raw).hexdigest()

def expected_pixel(i, pattern):
    if pattern == 0:
        return i % 16384
    if pattern == 1:
        levels = [0, 1, 2, 3, 4094, 4095, 4096, 4097,
                  8190, 8191, 8192, 8193, 12287, 12288, 16382, 16383]
        return levels[(i // WIDTH // 16) % 16]
    x = i + 1
    x = (x ^ (x << 13)) & 0xffffffff
    x ^= x >> 17
    x = (x ^ (x << 5)) & 0xffffffff
    return 8192 + x % 65 - 32

def summarize(values):
    n = len(values)
    mean = sum(values) / n
    step = 0
    unique = sorted(set(values))
    for a, b in zip(unique, unique[1:]):
        step = gcd(step, b - a)
    return dict(min=min(values), max=max(values), mean=mean,
                population_sd=sqrt(sum((x-mean)**2 for x in values)/n),
                unique=len(unique), unique_step_gcd=step,
                low2_counts=[sum((x & 3) == k for x in values) for k in range(4)],
                low4_counts=[sum((x & 15) == k for x in values) for k in range(16)])

def analyze(folder):
    report = {'scope': 'synthetic IFE probe; not sensor noise or image-quality proof', 'cases': {}}
    bypass_ok = True
    for name, pattern, nr in [('bypass_ramp', 0, 0), ('bypass_levels', 1, 0),
                              ('denoise_levels', 1, 1), ('bypass_noise', 2, 0),
                              ('denoise_noise', 2, 1)]:
        if not (folder / f'{name}_status.txt').exists():
            continue
        source, input_sha = read_raw(folder / f'{name}_input.raw')
        result, output_sha = read_raw(folder / f'{name}_output.raw')
        status = dict(line.split('=', 1) for line in
                      (folder / f'{name}_status.txt').read_text().splitlines() if '=' in line)
        if (status.get('ret') != '0' or status.get('output_valid') != '1' or
            status.get('nr') != str(nr) or status.get('pattern') != str(pattern)):
            raise ValueError(f'{name}: mismatched or failed hardware status')
        if any(v != expected_pixel(i, pattern) for i, v in enumerate(source)):
            raise ValueError(f'{name}: input does not match the expected test pattern')
        mismatch = sum(a != b for a, b in zip(source, result))
        case = dict(input_sha256=input_sha, output_sha256=output_sha,
                    mismatches=mismatch, max_abs_error=max(abs(a-b) for a,b in zip(source,result)),
                    input=summarize(source), output=summarize(result), status=status)
        case['exact_transform_matches'] = {
            'low12_only': all(b == (a & 4095) for a,b in zip(source,result)),
            'shift_right_2': all(b == (a >> 2) for a,b in zip(source,result)),
            'shift_left_2': all(b == ((a << 2) & 65535) for a,b in zip(source,result)),
        }
        if nr == 0:
            bypass_ok &= mismatch == 0
        if name == 'denoise_levels':
            # Crop 4 pixels from each 16-row band and the image edges.
            stable = [(a,b) for i,(a,b) in enumerate(zip(source,result))
                      if 4 <= i % WIDTH < WIDTH-4 and 4 <= (i//WIDTH)%16 < 12]
            case['flat_interior_mismatches'] = sum(a != b for a,b in stable)
        if name == 'denoise_noise':
            case['synthetic_sd_ratio'] = case['output']['population_sd'] / case['input']['population_sd']
        report['cases'][name] = case
    if not report['cases']:
        raise ValueError('No captured probe cases found')
    report['bypass_bit_exact'] = (bypass_ok and
        {'bypass_ramp', 'bypass_levels'}.issubset(report['cases']))
    report['denoise_tested'] = {'denoise_levels', 'denoise_noise'}.issubset(report['cases'])
    report['raw14_agc_integration_approved'] = False
    report['interpretation'] = (
        'Bypass preserves this RAW14 transport. Review NR levels/noise outputs and real RAW next.'
        if report['bypass_bit_exact'] else
        'STOP: bypass changes DN or required captures are missing. Do not move AGC or guess a shift.')
    return report

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('folder', type=Path)
    args = parser.parse_args()
    report = analyze(args.folder)
    destination = args.folder / 'analysis.json'
    if destination.exists():
        raise SystemExit('analysis.json already exists; preserve it or choose a new result directory')
    destination.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(report['interpretation'])
    print(destination)
    raise SystemExit(0 if report['bypass_bit_exact'] else 1)
