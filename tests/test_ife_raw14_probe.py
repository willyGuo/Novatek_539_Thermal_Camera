"""Host tests of configuration and evidence analysis; no hardware emulation verdict."""
import argparse
from array import array
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
probe = root / 'tools/ife_raw14_probe'
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', type=Path, required=True)
    args = parser.parse_args()
    prepare = load('probe_prepare', probe/'prepare_sdk.py')
    analyze = load('probe_analyze', probe/'analyze.py')
    with tempfile.TemporaryDirectory(prefix='ife_raw14_host_') as tmp:
        work = Path(tmp)
        prepare.prepare(args.sdk, work)
        cc = shutil.which('gcc')
        if not cc:
            raise SystemExit('gcc not found')
        command = [cc, '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-I', str(work),
                   '-I', str(probe), '-I', str(args.sdk/'hdal/drivers/k_driver/include'),
                   '-I', str(args.sdk/'vos/drivers/include'), str(root/'tests/test_ife_raw14_probe.c'),
                   '-o', str(work/'config.exe')]
        env = os.environ.copy()
        env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
        subprocess.run(command, check=True, env=env)
        subprocess.run([str(work/'config.exe'), str(work)], check=True, env=env)
        for pattern in range(3):
            values, _ = analyze.read_raw(work/f'pattern_{pattern}.raw')
            assert all(v == analyze.expected_pixel(i, pattern) for i,v in enumerate(values))
        # Analyze synthetic fixtures to ensure quantization cannot pass as identity.
        for name,pattern,nr in [('bypass_ramp',0,0),('bypass_levels',1,0),
                                ('denoise_levels',1,1),('bypass_noise',2,0),('denoise_noise',2,1)]:
            a = array('H', (analyze.expected_pixel(i,pattern) for i in range(640*480)))
            if sys.byteorder != 'little': a.byteswap()
            (work/f'{name}_input.raw').write_bytes(a.tobytes())
            (work/f'{name}_output.raw').write_bytes(a.tobytes())
            (work/f'{name}_status.txt').write_text(f'ret=0\noutput_valid=1\nnr={nr}\npattern={pattern}\n')
        assert analyze.analyze(work)['bypass_bit_exact']
        a = array('H', (analyze.expected_pixel(i,0) >> 2 for i in range(640*480)))
        if sys.byteorder != 'little': a.byteswap()
        (work/'bypass_ramp_output.raw').write_bytes(a.tobytes())
        report = analyze.analyze(work)
        assert not report['bypass_bit_exact']
        assert report['cases']['bypass_ramp']['exact_transform_matches']['shift_right_2']
        (work/'bypass_ramp_output.raw').write_bytes(b'\x00')
        try: analyze.analyze(work)
        except ValueError: pass
        else: raise AssertionError('truncated capture was accepted')
        print('PASS: analyzer rejects 14-to-12 bit truncation and incomplete captures')

if __name__ == '__main__': main()
