"""Run production DDE frame wrapper against a small HDAL memory mock (host only)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = shutil.which('gcc')
assert cc
env = os.environ.copy()
env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
with tempfile.TemporaryDirectory(prefix='dde_frame_') as directory:
    directory = Path(directory)
    (directory / 'hdal.h').write_text(r'''
#ifndef HOST_DDE_HDAL_H
#define HOST_DDE_HDAL_H
#include <stdint.h>
typedef uintptr_t UINTPTR;
typedef uint32_t UINT32;
#define HD_OK 0
#define HD_VIDEO_PXLFMT_YUV420 1
#define HD_COMMON_MEM_VB_INVALID_BLK ((UINTPTR)-1)
#define HD_COMMON_MEM_MEM_TYPE_CACHE 1
typedef struct {
    int pxlfmt;
    UINTPTR phy_addr[1];
    UINT32 loff[1];
    struct { UINT32 w, h; } dim;
    UINTPTR blk;
} HD_VIDEO_FRAME;
UINTPTR hd_common_mem_blk2pa(UINTPTR block);
void *hd_common_mem_mmap(int type, UINTPTR base, UINT32 bytes);
int hd_common_mem_flush_cache(void *va, UINT32 bytes);
int hd_common_mem_munmap(void *va, UINT32 bytes);
#endif
''', encoding='ascii')
    output = directory / ('test.exe' if os.name == 'nt' else 'test')
    sources = ['tests/test_dde_frame.c', 'Enhancement/DDE/dde_frame.c',
               'Enhancement/DDE/dde.c', 'Enhancement/DDE/dde_dt_opt.c']
    for simd in [0, 1]:
        subprocess.run([cc, '-std=gnu99', '-O3', '-Wall', '-Wextra', '-Werror',
                        f'-DDDE_SIMD4={simd}', '-I', str(directory), '-I', str(root),
                        *[str(root / path) for path in sources], '-pthread', '-lm',
                        '-o', str(output)], check=True, env=env)
        subprocess.run([str(output)], check=True, env=env)
