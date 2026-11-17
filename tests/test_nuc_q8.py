"""Host-only NUC regression; requires GCC and matching SDK headers."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--sdk', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
cc = shutil.which('gcc')
if not cc:
    raise SystemExit('gcc not found')
env = os.environ.copy()
env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
with tempfile.TemporaryDirectory(prefix='tiffany_nuc_q8_') as tmp:
    for profile in (1, 2):
        output = Path(tmp) / f'nuc_{profile}.exe'
        cmd = [cc, '-std=gnu99', '-O2', '-Wall', '-Wextra', '-D__LINUX',
               f'-DTHERMAL_SENSOR={profile}', '-ffunction-sections',
               '-fdata-sections', '-Wl,--gc-sections']
        for inc in ('hdal/include', 'hdal/vendor/media/include', 'vos/drivers/include'):
            cmd += ['-I', str(args.sdk / inc)]
        cmd += [str(root / 'tests/test_nuc_q8.c'), '-o', str(output)]
        subprocess.run(cmd, check=True, env=env)
        subprocess.run([str(output), str(Path(tmp) / f'gain_{profile}.bin')],
                       check=True, env=env)
        print(f'Profile {profile}: NUC Q8 passed', flush=True)
