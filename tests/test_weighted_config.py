"""Host CFG/menu reset integration, production parser/load/save/update paths.
python tests/test_weighted_config.py --sdk ../ns2501_linux_sdk-main/code
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--sdk', required=True, type=Path)
parser.add_argument('--cc', default='gcc')
parser.add_argument('--standard-cfg', type=Path,
                    default=Path(__file__).resolve().parent/'fixtures/standard_display.cfg')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
cc = shutil.which(args.cc)
assert cc
env = os.environ.copy()
env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH','')
source = (root/'Config/app_config.c').read_text(encoding='utf-8')
def section(start,end):
    return source[source.index(start):source.index(end)]
code = section('#include "app_config.h"','static void cfg_initialize_from_runtime(')
code += r"""
#include <assert.h>
static int fail_save;
static AGC_FIELDSCALE_PARAMS saved_fieldscale;
static AGC_HISTOGRAM_P1_P99_PARAMS saved_histogram;
static AGC_MODE current_mode = AGC_MODE_WEIGHTED_CPU;
static void cfg_initialize_from_runtime(void) { assert(g_app_config.initialized); }
static int cfg_apply_runtime(const APP_CONFIG_VALUES *p) {
    int ret=agc_weighted_set_params(&p->weighted);
    if (!ret) ret=raw_temporal_denoise_set_params(&p->raw_temporal);
    if (!ret) ret=dde_set_params(&p->dde);
    if (!ret) ret=clahe_set_params(&p->clahe);
    if(ret) return ret;
    if(!ret) current_mode=cfg_agc_selection(p);
    return ret;
}
static void app_config_request_agc(void);
int liveview_switch_agc(int mode) { current_mode=(AGC_MODE)mode;return 0; }
AGC_MODE agc_mode_get(void) { return current_mode; }
const char *agc_mode_name(AGC_MODE mode) { (void)mode;return "host-selected AGC"; }
static int app_config_histogram_p1_p99_menu_run(void) { abort(); }
static int app_config_fieldscale_menu_run(void) { abort(); }
int agc_fieldscale_validate_params(const AGC_FIELDSCALE_PARAMS *p) { (void)p; return 0; }
int agc_histogram_p1_p99_validate_params(const AGC_HISTOGRAM_P1_P99_PARAMS *p) { (void)p; return 0; }
int liveview_set_ffc_enabled(int n) { (void)n;abort(); }
void ffc_set_shading_enabled(int n) { (void)n;abort(); }
int liveview_apply_shutter_close_wait_ms(unsigned int n) { (void)n;abort(); }
int liveview_set_shutter_interval_sec(unsigned int n) { (void)n;abort(); }
int liveview_set_bpr_enabled(int n) { (void)n;abort(); }
int liveview_set_nuc_enabled(int n) { (void)n;abort(); }
void bpr_set_threshold(unsigned int n) { (void)n;abort(); }
int agc_histogram_p1_p99_set_params(const AGC_HISTOGRAM_P1_P99_PARAMS *p) { saved_histogram=*p;return 0; }
int agc_fieldscale_set_params(const AGC_FIELDSCALE_PARAMS *p) { saved_fieldscale=*p;return 0; }
int pseudo_request_mode(int n) { (void)n;abort(); }
"""
code += section('static int cfg_parse_long(', 'static int cfg_apply_runtime(')
code += section('int app_config_load(', 'int app_config_get_int(')
code += section('int app_config_get_int(', 'static void app_config_request_agc(')
code += section('static void app_config_request_agc(void)\n{', 'int app_config_update_int(')
code += section('int app_config_update_int(', 'int app_config_update_videoproc_isp(')
code += section('static int app_config_prompt_int(', 'static const char *app_config_output_mode_name(')
code += section('static int app_config_weighted_menu_run(', 'static int app_config_agc_menu_run(')
code += section('static int app_config_agc_menu_run(', 'static void app_config_set_dde_preset(')
code += section('static void app_config_set_dde_preset(', 'static int app_config_clahe_menu_run(')
code += section('static int app_config_apply_standard_display_mode(', 'int app_config_menu_run(')
code += source[source.index('int app_config_save(void)\n{'):]
code = code.replace('int app_config_save(void)\n{','int app_config_save(void)\n{\n    if (fail_save) return -1;')
code += r"""
static void initial(void) {
    memset(&g_app_config,0,sizeof(g_app_config)); g_app_config.initialized=1;
    g_app_config.boot_load=1; g_app_config.agc_weighted_enable=1;
    g_app_config.fps=25;
    g_app_config.clahe=(CLAHE_PARAMS){0,32,8};
    g_app_config.dde=(DDE_PARAMS){0,8,16,150,2,1};
    assert(clahe_set_params(&g_app_config.clahe)==0);
    assert(dde_set_params(&g_app_config.dde)==0);
    agc_weighted_default_params(&g_app_config.weighted);
    { RAW_TEMPORAL_DENOISE_PARAMS defaults = RAW_TEMPORAL_DENOISE_DEFAULTS;
      g_app_config.raw_temporal=defaults; }
    current_mode=AGC_MODE_WEIGHTED_CPU;
}
static void check_defaults(void) {
    AGC_WEIGHTED_PARAMS expected,runtime;
    agc_weighted_default_params(&expected); agc_weighted_get_params(&runtime);
    assert(memcmp(&expected,&g_app_config.weighted,sizeof(expected))==0);
    assert(memcmp(&expected,&runtime,sizeof(expected))==0);
    assert(g_app_config.agc_weighted_enable==1 && g_app_config.fps==25);
}
int main(void) {
    unsigned int i; AGC_WEIGHTED_PARAMS before,runtime;
    const char *bad[]={"nan","inf","-1","65","abc",""};
    initial(); assert(cfg_make_dir()==0);
    {
        APP_CONFIG_VALUES expected=g_app_config, before;
        DDE_PARAMS runtime_dde;
        CLAHE_PARAMS runtime_clahe;
        AGC_WEIGHTED_PARAMS runtime_weighted;
        FILE *fp=fopen(TEST_STANDARD_CFG_PATH,"r");char line[256];assert(fp);
        while(fgets(line,sizeof(line),fp)) {
            char *key=cfg_trim(line),*value=strchr(key,'=');
            if (!value) continue;
            *value++=0;key=cfg_trim(key);value=cfg_trim(value);
            if (!strncmp(key,"agc_",4)||!strncmp(key,"clahe_",6)||!strncmp(key,"dde_",4))
                (void)cfg_store_pair(&expected,key,value);
        }
        fclose(fp);
        g_app_config.clahe=(CLAHE_PARAMS){1,16,8};
        g_app_config.dde=(DDE_PARAMS){0,8,16,150,3,1};
        g_app_config.agc_fieldscale_enable=1;g_app_config.agc_weighted_enable=0;
        assert(clahe_set_params(&g_app_config.clahe)==0);
        assert(dde_set_params(&g_app_config.dde)==0);
        assert(agc_weighted_set_params(&g_app_config.weighted)==0);
        current_mode=AGC_MODE_FIELDSCALE_CPU;
        before=g_app_config;
        fail_save=1;assert(app_config_apply_standard_display_mode()==-1);
        assert(memcmp(&before,&g_app_config,sizeof(before))==0);
        dde_get_params(&runtime_dde);assert(!runtime_dde.enable && runtime_dde.sigma_s==8);
        clahe_get_params(&runtime_clahe);assert(runtime_clahe.enable && runtime_clahe.tile_size==16);
        assert(current_mode==AGC_MODE_FIELDSCALE_CPU);
        fail_save=0;
        /* Removed options must neither save nor change pending/runtime state. */
        {FILE *fp=fopen(TEST_MENU_PATH,"w");assert(fp);fputs("2\n3\n0\n",fp);fclose(fp);}
        assert(freopen(TEST_MENU_PATH,"r",stdin));assert(app_config_simple_display_menu_run()==0);
        assert(memcmp(&before,&g_app_config,sizeof(before))==0);
        {FILE *fp=fopen(TEST_MENU_PATH,"w");assert(fp);fputs("1\n0\n",fp);fclose(fp);}
        assert(freopen(TEST_MENU_PATH,"r",stdin));assert(app_config_simple_display_menu_run()==0);
        assert(current_mode==AGC_MODE_WEIGHTED_CPU && g_app_config.agc_weighted_enable==1);
        assert(!g_app_config.agc_fieldscale_enable && !g_app_config.agc_histogram_p1_p99_enable);
        agc_weighted_get_params(&runtime_weighted);
        assert(runtime_weighted.clip_tenths==expected.weighted.clip_tenths);
        assert(runtime_weighted.redistribute==expected.weighted.redistribute);
        assert(runtime_weighted.bright_weight==expected.weighted.bright_weight);
        assert(runtime_weighted.bright_percentile_tenths==expected.weighted.bright_percentile_tenths);
        assert(runtime_weighted.gain_limit_tenths==expected.weighted.gain_limit_tenths);
        assert(runtime_weighted.temporal_previous_percent==expected.weighted.temporal_previous_percent);
        dde_get_params(&runtime_dde);clahe_get_params(&runtime_clahe);
        assert(memcmp(&runtime_dde,&expected.dde,sizeof(runtime_dde))==0);
        assert(memcmp(&runtime_clahe,&expected.clahe,sizeof(runtime_clahe))==0);
        assert(memcmp(&saved_fieldscale,&expected.fieldscale,sizeof(saved_fieldscale))==0);
        assert(memcmp(&saved_histogram,&expected.histogram_p1_p99,sizeof(saved_histogram))==0);
        assert(g_app_config.fps==before.fps &&
               !memcmp(&g_app_config.raw_temporal,&before.raw_temporal,sizeof(before.raw_temporal)));
        initial();assert(app_config_load()==0);
        assert(g_app_config.agc_weighted_enable && !g_app_config.clahe.enable &&
               g_app_config.clahe.tile_size==expected.clahe.tile_size &&
               g_app_config.clahe.clip_limit==expected.clahe.clip_limit);
        assert(!memcmp(&g_app_config.dde,&expected.dde,sizeof(expected.dde)));
        assert(app_config_update_int("dde_base_scale",2)==0);
        dde_get_params(&runtime_dde);assert(runtime_dde.base_scale==2 && runtime_dde.num_iters==2);
        assert(app_config_update_int("dde_base_scale",0)==-1);
        assert(app_config_update_int("dde_base_scale",3)==-1);
        fail_save=1;assert(app_config_update_int("dde_base_scale",1)==-1);fail_save=0;
        dde_get_params(&runtime_dde);assert(runtime_dde.base_scale==2 && g_app_config.dde.base_scale==2);
        initial();assert(app_config_load()==0);
        dde_get_params(&runtime_dde);assert(runtime_dde.base_scale==2);
        {FILE *fp=fopen(TEST_MENU_PATH,"w");assert(fp);fputs("8\n1\n0\n",fp);fclose(fp);}
        assert(freopen(TEST_MENU_PATH,"r",stdin));assert(app_config_dde_menu_run()==0);
        dde_get_params(&runtime_dde);assert(runtime_dde.base_scale==1);
        puts("PASS: DDE base scale CFG/menu, validation, save/load/live apply and rollback");
        puts("PASS: Standard display matches reference CFG, save/load/runtime, rollback; Fast/AGC-only removed");
        initial();
    }
    {
        int n; RAW_TEMPORAL_DENOISE_PARAMS runtime_raw;
        assert(app_config_update_int("raw_temporal_denoise_enable",1)==0);
        assert(app_config_update_int("raw_temporal_denoise_threshold_dn",8)==0);
        assert(app_config_update_int("raw_temporal_denoise_history_shift",1)==0);
        initial(); assert(app_config_load()==0);
        raw_temporal_denoise_get_params(&runtime_raw);
        assert(runtime_raw.enable==1 && runtime_raw.threshold_dn==8 && runtime_raw.history_shift==1);
        assert(app_config_get_int("raw_temporal_denoise_threshold_dn",&n)==0 && n==8);
        assert(app_config_update_int("raw_temporal_denoise_threshold_dn",0)==-1);
        fail_save=1; assert(app_config_update_int("raw_temporal_denoise_enable",0)==-1); fail_save=0;
        raw_temporal_denoise_get_params(&runtime_raw); assert(runtime_raw.enable==1 && g_app_config.raw_temporal.enable==1);
        { FILE *fp=fopen(TEST_MENU_PATH,"w");assert(fp);fputs("4\n0\n",fp);fclose(fp); }
        assert(freopen(TEST_MENU_PATH,"r",stdin)); assert(app_config_raw_preprocessing_menu_run()==0);
        initial(); assert(app_config_load()==0); raw_temporal_denoise_get_params(&runtime_raw);
        assert(!runtime_raw.enable && runtime_raw.threshold_dn==12 && runtime_raw.history_shift==2);
        puts("PASS: RAW temporal CFG validation, save/load/runtime update, save failure rollback, menu defaults");
    }
    assert(agc_weighted_set_params(&g_app_config.weighted)==0);
    {
        int enabled;
        assert(!g_app_config.weighted.balanced_background);
        assert(app_config_update_int("agc_weighted_balanced_background",1)==0);
        agc_weighted_get_params(&runtime); assert(runtime.balanced_background==1);
        assert(app_config_get_int("agc_weighted_balanced_background",&enabled)==0 && enabled==1);
        initial(); assert(app_config_load()==0); assert(g_app_config.weighted.balanced_background==1);
        agc_weighted_get_params(&runtime); assert(runtime.balanced_background==1);
        assert(cfg_store_pair(&g_app_config,"agc_weighted_balanced_background","0.5")==-1);
        assert(app_config_update_int("agc_weighted_balanced_background",2)==-1);
        assert(app_config_update_int("agc_weighted_balanced_background",-1)==-1);
        fail_save=1; assert(app_config_update_int("agc_weighted_balanced_background",0)==-1);
        agc_weighted_get_params(&runtime); assert(runtime.balanced_background==1 && g_app_config.weighted.balanced_background==1);
        fail_save=0;
        { FILE *fp=fopen(TEST_MENU_PATH,"w");assert(fp);
          fprintf(fp,"%u\n0\n0\n",(unsigned int)cfg_weighted_index("agc_weighted_balanced_background")+2U);fclose(fp); }
        assert(freopen(TEST_MENU_PATH,"r",stdin)); assert(app_config_weighted_menu_run()==0);
        agc_weighted_get_params(&runtime); assert(!runtime.balanced_background);
        initial(); assert(app_config_load()==0); assert(!g_app_config.weighted.balanced_background);
        puts("PASS: Balanced Background CFG/menu switch, save/load/live apply/validation/rollback");
    }
    {
        const char *weights[]={"1.001","12.3456","1000","1e100","1.7976931348623157e308"};
        const char *invalid[]={"0.9","0","-1","nan","inf","1e309","12junk",""};
        for(i=0;i<sizeof(weights)/sizeof(weights[0]);i++) {
            double expected=strtod(weights[i],NULL);
            assert(cfg_store_pair(&g_app_config,"agc_weighted_bright_weight",weights[i])==0);
            assert(g_app_config.weighted.bright_weight==expected);
            assert(app_config_save()==0); initial(); assert(app_config_load()==0);
            agc_weighted_get_params(&runtime);
            assert(g_app_config.weighted.bright_weight==expected && runtime.bright_weight==expected);
        }
        for(i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
            before=g_app_config.weighted;
            assert(cfg_store_pair(&g_app_config,"agc_weighted_bright_weight",invalid[i])==-1);
            assert(memcmp(&before,&g_app_config.weighted,sizeof(before))==0);
        }
        { FILE *fp=fopen(TEST_MENU_PATH,"w");assert(fp);fputs("4\n12.3456\n0\n",fp);fclose(fp); }
        assert(freopen(TEST_MENU_PATH,"r",stdin)); assert(app_config_weighted_menu_run()==0);
        agc_weighted_get_params(&runtime); assert(runtime.bright_weight==12.3456);
        initial(); assert(app_config_load()==0); assert(g_app_config.weighted.bright_weight==12.3456);
        fail_save=1;
        { FILE *fp=fopen(TEST_MENU_PATH,"w");assert(fp);fputs("4\n1000\n0\n",fp);fclose(fp); }
        assert(freopen(TEST_MENU_PATH,"r",stdin)); assert(app_config_weighted_menu_run()==0);
        agc_weighted_get_params(&runtime); assert(runtime.bright_weight==12.3456);
        assert(g_app_config.weighted.bright_weight==12.3456);
        fail_save=0;
        initial(); assert(app_config_save()==0);
    }

    before=g_app_config.weighted;
    assert(app_config_save()==0); initial(); assert(app_config_load()==0);
    assert(memcmp(&before,&g_app_config.weighted,sizeof(before))==0);
    for(i=0;i<2;i++) {
        FILE *fp=fopen(APP_CONFIG_PATH,"w"); assert(fp);
        fprintf(fp,"agc_weighted_reset_defaults=%u\nagc_weighted_gain_limit=0.1\nagc_weighted_balanced_background=1\nagc_weighted_bright_percentile=21\nagc_weighted_reset_defaults=1\n",i);
        fclose(fp); assert(app_config_load()==0); check_defaults();
        fp=fopen(APP_CONFIG_PATH,"r"); assert(fp);
        { char text[16384]; size_t n=fread(text,1,sizeof(text)-1,fp); text[n]=0;
          assert(strstr(text,"agc_weighted_reset_defaults=0\n")); }
        fclose(fp);
    }
    assert(cfg_store_pair(&g_app_config,"agc_weighted_gain_limit","0.5")==0);
    assert(agc_weighted_set_params(&g_app_config.weighted)==0);
    before=g_app_config.weighted;
    fail_save=1; assert(app_config_update_int("agc_weighted_reset_defaults",1)==-1);
    assert(memcmp(&before,&g_app_config.weighted,sizeof(before))==0);
    agc_weighted_get_params(&runtime); assert(memcmp(&before,&runtime,sizeof(before))==0);
    { FILE *fp=fopen(APP_CONFIG_PATH,"w"); assert(fp); fputs("agc_weighted_reset_defaults=1\n",fp); fclose(fp); }
    assert(app_config_load()==-1);
    assert(memcmp(&before,&g_app_config.weighted,sizeof(before))==0);
    agc_weighted_get_params(&runtime); assert(memcmp(&before,&runtime,sizeof(before))==0);
    fail_save=0; assert(app_config_update_int("agc_weighted_reset_defaults",1)==0); check_defaults();
    assert(app_config_update_int("agc_weighted_reset_defaults",2)==-1);
    assert(app_config_update_int("agc_weighted_reset_defaults",0)==0); check_defaults();
    /* Feed the real CFG menu reset option followed by Back. */
    assert(cfg_store_pair(&g_app_config,"agc_weighted_gain_limit","0.5")==0);
    { FILE *fp=fopen(TEST_MENU_PATH,"w");assert(fp); fprintf(fp,"%u\n0\n",(unsigned int)WEIGHTED_CFG_COUNT+2);fclose(fp); }
    assert(freopen(TEST_MENU_PATH,"r",stdin)); assert(app_config_weighted_menu_run()==0); check_defaults();
    /* Deleted keys in an old file are ignored; saving removes them. */
    {FILE *fp=fopen(APP_CONFIG_PATH,"w");assert(fp);
     fputs("cfg_boot_load=1\npre_bnr_enable=1\nagc_weighted_enable=1\nagc_weighted_mode=1\nagc_v3_denoise_enable=1\nagc_min_max_tone_curve_enable=1\nraw_nlm_enable=1\nraw_nlm_h_dn=8\nraw_nlm_search_window=5\nagc_weighted_background_enable=1\nagc_weighted_background_low_percentile=10\nagc_weighted_background_high_percentile=90\nagc_weighted_background_target_gray=128\nagc_weighted_background_output_span=120\nagc_weighted_background_max_gain=1\nagc_weighted_large_scale_gain=8\n",fp);fclose(fp);}
    initial();assert(app_config_load()==0 && current_mode==AGC_MODE_WEIGHTED_CPU);
    assert(!g_app_config.weighted.balanced_background); /* Old CFG has no new key. */
    assert(app_config_save()==0);
    {FILE *fp=fopen(APP_CONFIG_PATH,"r");char text[16384];size_t n;assert(fp);
     n=fread(text,1,sizeof(text)-1,fp);text[n]=0;fclose(fp);
     assert(!strstr(text,"agc_weighted_mode")&&!strstr(text,"agc_v3_denoise"));
     assert(!strstr(text,"raw_nlm_"));
     assert(!strstr(text,"pre_bnr_enable"));
     assert(!strstr(text,"agc_min_max_tone_curve_enable"));
     assert(!strstr(text,"agc_weighted_background_")&&!strstr(text,"agc_weighted_large_scale_"));}
    {
        const char *keys[]={"agc_histogram_p1_p99_enable",
            "agc_fieldscale_enable","agc_weighted_enable"};
        const AGC_MODE modes[]={AGC_MODE_HISTOGRAM_P1_P99_CPU,
            AGC_MODE_FIELDSCALE_CPU,AGC_MODE_WEIGHTED_CPU};
        for(i=0;i<3;i++) {
            assert(app_config_update_int(keys[i],1)==0);
            assert(current_mode==modes[i]);
            assert(g_app_config.agc_histogram_p1_p99_enable+
                g_app_config.agc_fieldscale_enable+g_app_config.agc_weighted_enable==1);
        }
    }
    {
        APP_CONFIG_VALUES before=g_app_config;
        AGC_MODE previous=current_mode;
        FILE *fp=fopen(TEST_MENU_PATH,"w");assert(fp);fputs("2\n0\n",fp);fclose(fp);
        assert(freopen(TEST_MENU_PATH,"r",stdin));assert(app_config_agc_menu_run()==0);
        assert(memcmp(&before,&g_app_config,sizeof(before))==0 && current_mode==previous);
    }
    puts("PASS: Weighted CFG/menu, three-way exclusion, old-key migration, reset/persistence/save-failure rollback");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='tiffany_weighted_cfg_') as tmp:
    work=Path(tmp)
    standard=work/'standard.cfg';standard.write_bytes(args.standard_cfg.read_bytes())
    code=code.replace('#define APP_CONFIG_DIR "/mnt/sd"',f'#define APP_CONFIG_DIR "{work.as_posix()}/cfg"')
    code=code.replace('#define APP_CONFIG_PATH "/mnt/sd/tiffany_thermal.cfg"','#define APP_CONFIG_PATH APP_CONFIG_DIR "/tiffany_thermal.cfg"')
    extra=[]
    if os.name=='nt':
        code=code.replace('mkdir(APP_CONFIG_DIR, 0755)','mkdir(APP_CONFIG_DIR)').replace('fsync(','_commit(')
        code=code.replace('rename(APP_CONFIG_TEMP_PATH, APP_CONFIG_PATH)','host_replace(APP_CONFIG_TEMP_PATH, APP_CONFIG_PATH)')
        code='int host_replace(const char *,const char *);\n'+code
        compat=work/'compat.c'
        compat.write_text('#include <windows.h>\nint host_replace(const char *a,const char *b) { return MoveFileExA(a,b,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)?0:-1;}\n')
        extra=[str(compat)]
    path=work/'test.c';path.write_text(code,encoding='utf-8')
    for profile in (1,2):
        cmd=[cc,'-std=gnu99','-O2','-D__LINUX',f'-DTHERMAL_SENSOR={profile}',
             '-I',str(root),'-I',str(root/'Config'),f'-DTEST_MENU_PATH="{work.as_posix()}/menu.txt"',
             f'-DTEST_STANDARD_CFG_PATH="{standard.as_posix()}"']
        for inc in ['hdal/include','hdal/vendor/media/include','hdal/vendor/isp/include','hdal/vendor/cv/include','vos/drivers/include']:
            cmd+=['-I',str(args.sdk.resolve()/inc)]
        exe=work/f'test_{profile}.exe'
        subprocess.run(cmd+[str(path),str(root/'Enhancement/AGC/agc_weighted.c'),str(root/'Enhancement/RAW/raw_temporal_denoise.c'),
            str(root/'Enhancement/CLAHE/clahe.c'),str(root/'Enhancement/DDE/dde.c'),
            str(root/'Enhancement/DDE/dde_dt_opt.c'),*extra,'-pthread','-lm','-o',str(exe)],check=True,env=env)
        completed=subprocess.run([str(exe)],env=env,capture_output=True,text=True)
        if completed.returncode:
            print(completed.stdout)
            print(completed.stderr)
            completed.check_returncode()
        assert 'PASS: RAW temporal CFG validation' in completed.stdout
        assert 'PASS: Balanced Background CFG/menu switch' in completed.stdout
        assert 'PASS: Standard display matches reference CFG' in completed.stdout
        assert 'PASS: DDE base scale CFG/menu' in completed.stdout
        print(f'Profile {profile}: PASS: DDE base scale CFG/menu, persistence/live apply/rollback')
        print(f'Profile {profile}: PASS: Standard snapshot, save/load/runtime/rollback; removed menu options')
        print(f'Profile {profile}: PASS: RAW temporal CFG save/load/live apply, validation, rollback and menu reset')
        print(f'Profile {profile}: PASS: Balanced Background switch, CFG/menu persistence, validation/reset and rollback')
        print(f'Profile {profile}: '+completed.stdout.splitlines()[-1])
