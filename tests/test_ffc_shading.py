"""Run host FFC regression using real SDK headers and a mocked GFX engine.

python tests/test_ffc_shading.py --sdk <SDK code directory>
No SDK library or board access is used; generated files stay in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
env = os.environ.copy()
compiler = shutil.which(args.cc)
if not compiler:
    raise SystemExit(f'Compiler not found: {args.cc}')
env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
includes = ['hdal/include', 'hdal/vendor/media/include', 'vos/drivers/include']
with tempfile.TemporaryDirectory(prefix='tiffany_ffc_') as tmp:
    work = Path(tmp)
    source = (root / 'Pipeline/liveview.c').read_text(encoding='utf-8')
    capture = source[source.index('static int liveview_capture_ffc_average('):
        source.index('static int liveview_run_ffc_calibration(')]
    cycle = source[source.index('static int liveview_shading_discard_frames('):
        source.index('static int liveview_run_shutter_calibration_cycle(')]
    worker = work / 'worker.inc'
    worker.write_text(capture + cycle, encoding='utf-8')
    extra = []
    if os.name == 'nt':
        compat = work / 'replace.c'
        compat.write_text('#include <windows.h>\n'
            'int host_replace(const char *a, const char *b) {\n'
            'return MoveFileExA(a,b,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) ? 0 : -1; }\n')
        extra = [str(compat)]
    for profile in (1, 2):
        output = work / f'test_{profile}.exe'
        data = work / f'profile_{profile}'
        cmd = [compiler, '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-D__LINUX',
            f'-DTHERMAL_SENSOR={profile}', f'-DFFC_SHADING_DIR="{data.as_posix()}"']
        for inc in includes:
            cmd += ['-I', str(args.sdk / inc)]
        cmd += [str(root / 'tests/test_ffc_shading.c'), *extra, '-pthread', '-o', str(output)]
        subprocess.run(cmd, check=True, env=env)
        subprocess.run([str(output)], check=True, env=env)
        print(f'Profile {profile}: host FFC regression passed', flush=True)
        worker_cmd = cmd.copy()
        worker_cmd[worker_cmd.index(str(root / 'tests/test_ffc_shading.c'))] = str(root / 'tests/test_ffc_shading_worker.c')
        worker_cmd[worker_cmd.index(f'-DFFC_SHADING_DIR="{data.as_posix()}"')] = f'-DFFC_SHADING_DIR="{data.as_posix()}_worker"'
        worker_cmd += [f'-DFFC_WORKER_SOURCE="{worker.as_posix()}"']
        subprocess.run(worker_cmd, check=True, env=env)
        subprocess.run([str(output)], check=True, env=env)
        print(f'Profile {profile}: RAW worker regression passed', flush=True)
