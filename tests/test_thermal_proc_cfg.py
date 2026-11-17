"""Exercise production VideoProc initialization with each HDAL call failing.

Host boundary mocks only; no SDK link, camera access or board validation.
Run: python tests/test_thermal_proc_cfg.py
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "Pipeline/liveview.c").read_text(encoding="utf-8")
start = source.index("static HD_RESULT set_proc_cfg(")
end = source.index("static HD_RESULT set_proc_param(", start)
function = source[start:end]
harness = r"""
#include <assert.h>
#include <stdio.h>
typedef unsigned int UINT32;
typedef int HD_RESULT;
typedef unsigned int HD_PATH_ID;
typedef unsigned int HD_OUT_ID;
typedef void VOID;
typedef struct { UINT32 w, h; } HD_DIM;
#define HD_OK 0
#define TRUE 1
#define HD_VIDEOPROC_PIPE_RAWALL 11
#define HD_VIDEO_FRC_RATIO(a,b) (((a)<<16)|(b))
#define CAP_OUT_FMT 16
#define HD_VIDEOPROC_0_OUT_4 44
enum { HD_VIDEOPROC_PARAM_DEV_CONFIG = 1, HD_VIDEOPROC_PARAM_FUNC_CONFIG,
       VENDOR_VIDEOPROC_PARAM_THERMAL_INFO, HD_VIDEOPROC_PARAM_CTRL,
       HD_VIDEOPROC_PARAM_LL_CONFIG };
typedef struct {
    UINT32 pipe, isp_id;
    struct { UINT32 func; } ctrl_max;
    struct { UINT32 func, pxlfmt, frc; HD_DIM dim; } in_max;
} HD_VIDEOPROC_DEV_CONFIG;
typedef struct { UINT32 func, ref_path_3dnr; } HD_VIDEOPROC_CTRL;
typedef struct { UINT32 delay_trig_lowlatency; } HD_VIDEOPROC_LL_CONFIG;
typedef struct { UINT32 in_func; } HD_VIDEOPROC_FUNC_CONFIG;
typedef struct { UINT32 enable; } VENDOR_VIDEOPROC_THERMAL_INFO;
static const UINT32 g_thermal_pipeline_id = 4;
static UINT32 calls, fail_at, closed, fail_close, nr3d, with_dim;
static UINT32 liveview_get_videoproc_alg_func(void) { return 0x1234; }
static int videoproc_isp_is_3dnr_enabled(void) { return nr3d; }
static int videoproc_isp_is_af_enabled(void) { return 0; }
static int videoproc_isp_is_wdr_enabled(void) { return 0; }
static int videoproc_isp_is_defog_enabled(void) { return 0; }
static HD_RESULT result(void) { calls++; return calls == fail_at ? -(int)calls : HD_OK; }
static HD_RESULT hd_videoproc_open(UINT32 in, HD_OUT_ID out, HD_PATH_ID *path) {
    HD_RESULT ret = result();
    assert(in == 0 && out == 77);
    if (ret == HD_OK) *path = 99;
    return ret;
}
static HD_RESULT hd_videoproc_close(HD_PATH_ID path) {
    assert(path == 99); closed++; return fail_close ? -99 : HD_OK;
}
static HD_RESULT hd_videoproc_set(HD_PATH_ID path, UINT32 item, void *data) {
    assert(path == 99);
    assert(item == calls - (with_dim ? 0 : -1));
    switch (item) {
    case HD_VIDEOPROC_PARAM_DEV_CONFIG: {
        HD_VIDEOPROC_DEV_CONFIG *p = data;
        assert(p->pipe == HD_VIDEOPROC_PIPE_RAWALL && p->isp_id == 4);
        assert(p->in_max.dim.w == 640 && p->in_max.dim.h == 480);
        assert(p->ctrl_max.func == 0x1234 && p->in_max.pxlfmt == CAP_OUT_FMT);
        break;
    }
    case HD_VIDEOPROC_PARAM_FUNC_CONFIG:
        assert(((HD_VIDEOPROC_FUNC_CONFIG *)data)->in_func == 0); break;
    case HD_VIDEOPROC_PARAM_CTRL: {
        HD_VIDEOPROC_CTRL *p = data;
        assert(p->func == 0x1234);
        assert(p->ref_path_3dnr == (nr3d ? HD_VIDEOPROC_0_OUT_4 : 0)); break;
    }
    case HD_VIDEOPROC_PARAM_LL_CONFIG:
        assert(((HD_VIDEOPROC_LL_CONFIG *)data)->delay_trig_lowlatency == 0); break;
    default: assert(0);
    }
    return result();
}
static HD_RESULT vendor_videoproc_set(HD_PATH_ID path, UINT32 item, VOID *data) {
    assert(path == 99 && item == VENDOR_VIDEOPROC_PARAM_THERMAL_INFO);
    assert(calls == (with_dim ? 3U : 2U));
    assert(((VENDOR_VIDEOPROC_THERMAL_INFO *)data)->enable == TRUE);
    return result();
}
FUNCTION
int main(void) {
    HD_DIM dim = {640, 480};
    UINT32 total, n = 0;
    for (with_dim = 0; with_dim <= 1; with_dim++)
    for (nr3d = 0; nr3d <= 1; nr3d++)
    for (fail_close = 0; fail_close <= 1; fail_close++) {
        total = with_dim ? 6 : 5;
        for (fail_at = 0; fail_at <= total; fail_at++) {
            HD_PATH_ID path = 88;
            calls = closed = 0;
            HD_RESULT ret = set_proc_cfg(&path, with_dim ? &dim : NULL, 77);
            if (fail_at) {
                assert(ret == -(int)fail_at && calls == fail_at);
                assert(path == 0 && closed == (fail_at > 1));
            } else {
                assert(ret == HD_OK && calls == total && path == 99 && closed == 0);
            }
            n++;
        }
    }
    printf("PASS: %u VideoProc init cases; success, all call failures, "
           "original error preservation and handle cleanup\n", n);
    return 0;
}
""".replace("FUNCTION", function)
cc = shutil.which("gcc")
if not cc:
    raise SystemExit("gcc required")
env = os.environ.copy()
env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
with tempfile.TemporaryDirectory(prefix="thermal_proc_cfg_") as temp:
    work = Path(temp)
    c_file = work / "thermal_proc_cfg.c"
    exe = work / ("thermal_proc_cfg.exe" if os.name == "nt" else "thermal_proc_cfg")
    c_file.write_text(harness, encoding="utf-8")
    subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", str(c_file), "-o", str(exe)], check=True, env=env)
    result = subprocess.run([str(exe)], check=True, env=env, capture_output=True, text=True)
    print(result.stdout.splitlines()[-1])
