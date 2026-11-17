"""Host regression of the complete NT98539A IQ IPE submission function.

Extracts production C from --sdk, compiles it with boundary/type mocks, and
compares it with the same function before the thermal-only guard. This does
not compile/link an SDK module or read camera registers.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def extract_function(source, name):
    start = source.index(f"static ER {name}(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk", type=Path, required=True, help="ATTO640 SDK code directory")
args = parser.parse_args()
relative = Path("hdal/vendor/isp/drivers/source/iq_539a/iq_flow/iq_flow.c")
source = (args.sdk / relative).read_text(encoding="utf-8")
function = extract_function(source, "iq_flow_set_ipe_param")
guard_start = function.index("\t/* Thermal samples are monochrome,")
guard_end = function.index("\tisp_api_set_iq_param", guard_start)
baseline = function[:guard_start] + function[guard_end:]
baseline = baseline.replace("iq_flow_set_ipe_param(", "baseline_set_ipe_param(", 1)

# These mocks describe only fields touched by the complete production function;
# they deliberately make no assertion about the SDK ABI or register layout.
ipp_names = sorted(set(re.findall(r"final_ipp\.(\w+)", function)) - {"ready_flag", "ipe_3dcc"})
ipe_names = sorted(set(re.findall(r"final_setting\.ipe\.(\w+)", function)))
low_names = sorted(set(re.findall(r"low_power_param\.(\w+)", function)))
param_names = sorted(set(re.findall(r"iq_param\[iq_info->id\]->(\w+)", function)))
defaults = sorted(set(re.findall(r"\bctl_ipe_\w+_init\b", function)))

declarations = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef unsigned int UINT32;
typedef int BOOL;
typedef int ER;
#define TRUE 1
#define FALSE 0
#define E_OK 0
#define E_PAR (-1)
#define IQ_DBG_WRN_MSG 1
#define IQ_TBL_IPE_GAMMA 1
#define IQ_TBL_IPE_YCURVE 2
#define ISP_SEN_MODE_THERMAL 9
#define ISP_IQ_ITEM_IPE_PARAM 17
#define PRINT_IQ_WRN(...) ((void)0)
typedef struct {
    UINT32 enable, motion_enable, edge_ker_mode, edge_mode;
    UINT32 cfa_enable, air_enable, lce_enable, set1_en, set2_en, set3_en;
    struct { UINT32 enable; } color_wet_set[4];
    struct { UINT32 fcs_enable; } cfa_fcs;
} MockParam;
static UINT32 submissions, submitted_id, logs;
static void mock_log(const char *fmt, ...) {
    char text[180];
    va_list ap;
    va_start(ap, fmt); vsnprintf(text, sizeof(text), fmt, ap); va_end(ap);
    assert(strstr(text, "RGBLPF=0 CFA=0 (forced bypass)") != NULL);
    logs++;
}
#define DBG_DUMP mock_log
"""
declarations += "typedef struct { MockParam *" + ", *".join(ipe_names) + "; } MockIpe;\n"
declarations += "typedef struct { UINT32 ready_flag; MockParam *ipe_3dcc; MockParam " + ", ".join(ipp_names) + "; } MockIpp;\n"
declarations += "typedef struct { UINT32 " + ", ".join(low_names) + "; } MockLow;\n"
declarations += "typedef struct { MockParam *" + ", *".join(param_names) + "; } MockIqParam;\n"
declarations += "static MockParam " + ", ".join(defaults) + ";\n"
declarations += r"""
typedef struct {
    UINT32 id, low_power_lv, ipp_tab_update, ipp_cnt, _3dcc_en;
    struct { UINT32 mode_type, reset, ipp_info; } ipp_trig_obj;
    struct { MockIpe ipe; } final_setting;
    MockIpp final_ipp;
    MockLow low_power_param;
} IQALG_INFO;
static MockIqParam params, *iq_param[8];
static MockParam configured;
static void isp_api_set_iq_param(UINT32 id, UINT32 item, MockIpe *ipe) {
    assert(item == ISP_IQ_ITEM_IPE_PARAM);
    assert(ipe->p_rgb_lpf != NULL && ipe->p_cfa != NULL);
    submissions++; submitted_id = id;
}
"""
initializers = "\n".join(f"    {name} = configured;" for name in defaults)
initializers += "\n" + "\n".join(f"    params.{name} = &configured;" for name in param_names)
initializers += "\n" + "\n".join(f"    a.final_ipp.{name} = configured;" for name in ipp_names)
harness = r"""
int main(void) {
    IQALG_INFO a, b;
    MockIpp expected;
    UINT32 mode, rgblpf, cfa, level, override_rgb, override_cfa, n = 0;
    memset(&configured, 0, sizeof(configured));
    configured.enable = configured.cfa_enable = configured.air_enable = 1;
    configured.motion_enable = configured.lce_enable = 1;
    configured.set1_en = configured.set2_en = configured.set3_en = 1;
    configured.edge_ker_mode = 2; configured.edge_mode = 3;
    for (mode = 0; mode < 2; mode++)
    for (rgblpf = 0; rgblpf < 2; rgblpf++)
    for (cfa = 0; cfa < 2; cfa++)
    for (override_rgb = 0; override_rgb <= 5; override_rgb++)
    for (override_cfa = 0; override_cfa <= 5; override_cfa++)
    /* Always return to level 0 so the SDK default-restore branch is tested. */
    for (level = 0; level < 12; level++) {
        memset(&a, 0, sizeof(a));
INITIALIZERS
        a.id = (n % 2) ? 4 : 1;
        iq_param[a.id] = &params;
        a.final_ipp.ready_flag = TRUE;
        a.final_ipp.ipe_rgblpf.enable = rgblpf;
        a.final_ipp.ipe_cfa.cfa_enable = cfa;
        a.ipp_trig_obj.ipp_info = n % 3;
        a.ipp_trig_obj.mode_type = mode ? ISP_SEN_MODE_THERMAL : 0;
        a.ipp_cnt = (n % 3 == 0) ? 1 : 50;
        a.ipp_trig_obj.reset = n % 7 == 0;
        a.ipp_tab_update = n % 4;
        a.low_power_lv = (level % 2) ? 0 : level / 2;
        a.low_power_param.rgblpf = override_rgb;
        a.low_power_param.cfa = override_cfa;
        b = a;
        submissions = logs = 0;
        assert(baseline_set_ipe_param(&b) == E_OK);
        assert(submissions == 1 && logs == 0);
        expected = b.final_ipp;
        if (mode) {
            expected.ipe_rgblpf.enable = FALSE;
            expected.ipe_cfa.cfa_enable = FALSE;
        }
        submissions = logs = 0;
        assert(iq_flow_set_ipe_param(&a) == E_OK);
        assert(submissions == 1);
        assert(submitted_id == (a.ipp_trig_obj.ipp_info << 16) + a.id);
        assert(memcmp(&a.final_ipp, &expected, sizeof(expected)) == 0);
        assert(logs == (mode && (a.ipp_cnt == 1 || a.ipp_trig_obj.reset)));
        n++;
    }
    a.final_ipp.ready_flag = FALSE;
    submissions = logs = 0;
    assert(iq_flow_set_ipe_param(&a) == E_PAR);
    assert(submissions == 0 && logs == 0);
    printf("PASS: %u IPE submissions; thermal bypass, low-power restore, "
           "nonthermal parity, ID routing and not-ready rejection\n", n);
    return 0;
}
""".replace("INITIALIZERS", initializers)
cc = shutil.which("gcc")
if not cc:
    raise SystemExit("gcc required")
env = os.environ.copy()
env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
with tempfile.TemporaryDirectory(prefix="thermal_ipe_") as temp:
    work = Path(temp)
    c_file = work / "thermal_ipe.c"
    exe = work / ("thermal_ipe.exe" if os.name == "nt" else "thermal_ipe")
    c_file.write_text(declarations + baseline + "\n" + function + harness, encoding="utf-8")
    subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", str(c_file), "-o", str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)
