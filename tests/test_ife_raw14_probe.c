#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "probe_config.h"

int main(int argc, char **argv)
{
    KDRV_IFE_IO_CFG io;
    KDRV_IFE_IQ_CFG iq;
    unsigned int i, seen[16384] = {0};
    unsigned int pattern;
    assert(argc == 2);
    probe_configure(&io, &iq, 0x1000, 0x2000, 0x3000, 0x4000, 0);
    assert(io.mode == KDRV_IPP_OPMODE_D2D);
    assert(KDRV_IPP_FMT_BPP(io.in_frm.fmt) == 16);
    assert(io.in_frm.fmt & KDRV_IPP_FMT_PACK_LSB);
    assert(io.out_frm.fmt == io.in_frm.fmt);
    assert(io.out_frm.phyaddr[0] == 0x4000 && io.in_frm.phyaddr[0] == 0x2000);
    assert(io.out_frm.lofs[0] == 1280 && io.ife_strp_num == 1);
    assert(!io.thermal_mode_en && !iq.fcurve.enable && !iq.filt.enable);
    assert(!iq.dgain.enable && !iq.cgain.enable && !iq.wdr.wdr_enable);
    probe_configure(&io, &iq, 0x1000, 0x2000, 0x3000, 0x4000, 1);
    assert(iq.filt.enable && !iq.filt.motion_set.motion_en && !iq.filt.rbfill.enable);
    assert(iq.filt.spatial.weight[0] == 31);
    for (i=0; i<PROBE_PIXELS; ++i) {
        ++seen[probe_pixel(i, 0)];
        assert(probe_pixel(i, 1) < 16384);
        assert(probe_pixel(i, 2) >= 8160 && probe_pixel(i, 2) <= 8224);
    }
    for (i=0; i<16384; ++i) assert(seen[i]);
    for (pattern=0; pattern<3; ++pattern) {
        char path[1024];
        FILE *fp;
        snprintf(path, sizeof(path), "%s/pattern_%u.raw", argv[1], pattern);
        fp = fopen(path, "wb");
        assert(fp);
        for (i=0; i<PROBE_PIXELS; ++i) {
            unsigned short v = probe_pixel(i, pattern);
            assert(fputc(v & 255, fp) != EOF);
            assert(fputc(v >> 8, fp) != EOF);
        }
        assert(fclose(fp) == 0);
    }
    puts("PASS: actual SDK config types, RAW16 LSB, D2D, NR isolation, full DN coverage");
    return 0;
}
