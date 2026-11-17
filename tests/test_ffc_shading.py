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
    capture = source[source.index('static void liveview_sync_ffc_upstream('):
        source.index('static int liveview_shading_discard_frames(')]
    cycle = source[source.index('static int liveview_shading_discard_frames('):
        source.index('static int liveview_apply_agc_histogram_p1_p99(')]
    worker = work / 'worker.inc'
    worker.write_text(capture + cycle, encoding='utf-8')
    extra = []
    if os.name == 'nt':
        compat = work / 'replace.c'
        compat.write_text('#include <windows.h>\n'
            'int host_replace(const char *a, const char *b) {\n'
            'return MoveFileExA(a,b,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) ? 0 : -1; }\n')
        extra = [str(compat)]
    # Compile complete modified translation units as well as extracted workers.
    # Windows shims only adapt POSIX declarations, never GFX/HDAL interfaces.
    isp = work / 'isp'
    isp.mkdir()
    for header in (args.sdk / 'hdal/vendor/isp/drivers/include').glob('*.h'):
        shutil.copy2(header, isp / header.name)
    for name in ('vendor_isp.h', 'vendor_vpe.h'):
        shutil.copy2(args.sdk / 'hdal/vendor/isp/include' / name, isp / name)
    syntax_flags = ['-I', str(isp)]
    if os.name == 'nt':
        (work / 'sys').mkdir()
        (work / 'sys/statvfs.h').write_text(
            'struct statvfs { unsigned long f_bsize, f_frsize, f_blocks, f_bfree, f_bavail; };\n'
            'int statvfs(const char *, struct statvfs *);\n')
        compat_header = work / 'posix_compat.h'
        compat_header.write_text('#include <direct.h>\n#include <io.h>\n'
            '#define mkdir(path, mode) _mkdir(path)\n#define fsync(fd) _commit(fd)\n')
        syntax_flags += ['-I', str(work), '-include', str(compat_header)]
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
        syntax_cmd = [compiler, '-std=gnu99', '-fsyntax-only', '-D__LINUX',
            f'-DTHERMAL_SENSOR={profile}', *syntax_flags]
        for inc in includes:
            syntax_cmd += ['-I', str(args.sdk / inc)]
        syntax_cmd += [str(root / path) for path in (
            'Pipeline/liveview.c', 'Correction/NUC/nuc.c',
            'Correction/FFC/ffc.c', 'Correction/TDC/tdc.c',
            'Config/app_config.c')]
        subprocess.run(syntax_cmd, check=True, env=env)
        print(f'Profile {profile}: complete correction pipeline host syntax passed', flush=True)
