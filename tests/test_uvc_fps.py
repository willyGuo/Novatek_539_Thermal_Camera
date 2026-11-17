"""Host test of production UVC open/descriptors/callbacks using matched SDK types."""
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
source = (root / 'Pipeline/UVC/uvc_output.c').read_text(encoding='utf-8')
prefix = source[:source.index('static int uvc_mjpeg_snapshot_is_pending(')]
# Target Linux is LP64; Windows GCC is LLP64. Preserve SDK field declarations,
# but widen the UVAC pointer-carrying ULONG argument in this host-only wrapper.
prefix = prefix.replace('#include "UVAC.h"',
    '#include "kwrap/type.h"\n#if defined(_WIN32)\n#define ULONG uintptr_t\n#endif\n#include "UVAC.h"')
opening = source[source.index('HD_RESULT uvc_output_open('):source.index('HD_RESULT uvc_output_start(')]
code = prefix + r'''
#include <assert.h>
#include <stdlib.h>
static unsigned int expected_fps, api_calls, descriptions;
static void uvc_cancel_mjpeg_snapshot(void) {}
static void uvc_output_reset(UVC_OUTPUT *o) { memset(o, 0, sizeof(*o)); }
HD_RESULT hd_videoenc_open(HD_IN_ID in, HD_OUT_ID out, HD_PATH_ID *path) {
    (void)in; (void)out; *path = 77; ++api_calls; return HD_OK;
}
HD_RESULT hd_videoenc_close(HD_PATH_ID path) { assert(path == 77); return HD_OK; }
HD_RESULT hd_videoenc_set(HD_PATH_ID path, HD_VIDEOENC_PARAM_ID id, VOID *data) {
    assert(path == 77); ++api_calls;
    if (id == HD_VIDEOENC_PARAM_IN) {
        HD_VIDEOENC_IN *p = data;
        assert(p->dim.w == THERMAL_SENSOR_WIDTH && p->dim.h == THERMAL_SENSOR_HEIGHT);
        assert(p->frc == HD_VIDEO_FRC_RATIO(1, 1));
    }
    return HD_OK;
}
HD_RESULT hd_common_mem_alloc(CHAR *name, UINTPTR *pa, VOID **va, UINT32 size, HD_COMMON_MEM_DDR_ID ddr) {
    (void)name; (void)ddr; *va = malloc(size); assert(*va);
    *pa = (UINTPTR)*va; ++api_calls; return HD_OK;
}
HD_RESULT hd_common_mem_free(UINTPTR pa, VOID *va) { (void)pa; free(va); return HD_OK; }
UINT32 UVAC_GetNeedMemSize(void) { return 4096; }
void UVAC_SetConfig(UVAC_CONFIG_ID id, ULONG value) {
    if (id == UVAC_CONFIG_UVC_H264_FRM_INFO || id == UVAC_CONFIG_UVC_HS_H264_FRM_INFO ||
        id == UVAC_CONFIG_UVC_MJPG_FRM_INFO || id == UVAC_CONFIG_UVC_HS_MJPG_FRM_INFO) {
        UVAC_VID_RESO_ARY *a = (void *)value;
        assert(a->aryCnt == 1 && a->pVidResAry[0].fpsCnt == 1);
        assert(a->pVidResAry[0].fps[0] == expected_fps);
        assert(a->pVidResAry[0].width == THERMAL_SENSOR_WIDTH);
        assert(a->pVidResAry[0].height == THERMAL_SENSOR_HEIGHT);
        ++descriptions;
    }
}
UINT32 UVAC_Open(UVAC_INFO *p) {
    assert(descriptions == 4 && p->strmInfo.strmFps == expected_fps);
    assert(p->strmInfo.strmCodec == UVAC_VIDEO_FORMAT_MJPG);
    ++api_calls; return E_OK;
}
''' + opening + r'''
int main(void) {
    UVC_OUTPUT o = {0};
    UVAC_STRM_INFO request = {0};
    unsigned int sequence[] = {30, 60, 30};
    assert(uvc_output_open(&o, 640, 480, 0, 4) != HD_OK);
    assert(uvc_output_open(&o, 640, 480, 61, 4) != HD_OK);
    assert(uvc_output_open(&o, 0, 480, 60, 4) != HD_OK);
    assert(uvc_output_open(NULL, 640, 480, 60, 4) != HD_OK);
    assert(api_calls == 0);
    for (unsigned int i=0; i<3; ++i) {
        expected_fps = sequence[i]; descriptions = 0;
        assert(uvc_output_open(&o, THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT, expected_fps, 4) == HD_OK);
        assert(o.fps == expected_fps && o.uvac_opened);
        request.strmWidth = THERMAL_SENSOR_WIDTH;
        request.strmHeight = THERMAL_SENSOR_HEIGHT;
        request.strmCodec = UVAC_VIDEO_FORMAT_MJPG;
        request.strmFps = expected_fps;
        assert(uvc_start_video_cb(0, &request) == E_OK);
        request.strmFps = expected_fps == 30 ? 60 : 30;
        assert(uvc_start_video_cb(0, &request) != E_OK);
        uvc_stop_video_cb(1);
        hd_common_mem_free(o.frame_pa, (void *)o.frame_va);
        hd_common_mem_free(o.uvac_pa, (void *)o.uvac_va);
        hd_videoenc_close(o.enc_path);
        g_uvc_output = NULL;
    }
    puts("PASS: 30 -> 60 -> 30, four descriptor tables, UVAC default, host negotiation, invalid FPS");
    return 0;
}
'''
cc = shutil.which('gcc')
if not cc:
    raise SystemExit('gcc not found')
env = os.environ.copy()
env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
with tempfile.TemporaryDirectory(prefix='uvc_fps_') as tmp:
    work = Path(tmp)
    c = work / 'test.c'
    c.write_text(code, encoding='utf-8')
    for profile in (1, 2):
        output = work / f'fps_{profile}.exe'
        command = [cc, '-std=gnu99', '-D__LINUX', f'-DTHERMAL_SENSOR={profile}',
                   '-I', str(root / 'Pipeline/UVC'), '-I', str(root / 'Config')]
        for inc in ('hdal/include', 'lib/include', 'vos/drivers/include'):
            command += ['-I', str(args.sdk / inc)]
        command += [str(c), '-pthread', '-o', str(output)]
        subprocess.run(command, check=True, env=env)
        subprocess.run([str(output)], check=True, env=env)
        print(f'Profile {profile}: UVC FPS host test passed', flush=True)
