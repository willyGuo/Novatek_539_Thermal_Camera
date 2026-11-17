#ifndef TIFFANY_IFE_PROBE_CONFIG_H
#define TIFFANY_IFE_PROBE_CONFIG_H
#include "kdrv_videoprocess/kdrv_ife_539a.h"
#include "probe_core.h"
#include "probe_filter_generated.h"

/* RAW16 LSB is the transport. No >>2 or automatic gain is performed here. */
static void probe_configure(KDRV_IFE_IO_CFG *io, KDRV_IFE_IQ_CFG *iq,
    unsigned long in_va, unsigned long in_pa,
    unsigned long out_va, unsigned long out_pa, int nr)
{
    memset(io, 0, sizeof(*io));
    memset(iq, 0, sizeof(*iq));
    io->mode = KDRV_IPP_OPMODE_D2D;
    io->ipp_status = KDRV_IPP_RUNNING;
    io->in_frm.fmt = KDRV_IPP_FMT_RAW16 | KDRV_IPP_FMT_PACK_LSB | KDRV_IPP_FMT_RGGB_R;
    io->in_frm.pxl_fmt = KDRV_IPP_PXLFMT_RGGB_R | KDRV_IPP_PXLFMT_PACK_LSB;
    io->in_frm.size.w = PROBE_W;
    io->in_frm.size.h = PROBE_H;
    io->in_frm.addr[0] = in_va;
    io->in_frm.phyaddr[0] = in_pa;
    io->in_frm.lofs[0] = PROBE_W * 2U;
    io->out_frm = io->in_frm;
    io->out_frm.addr[0] = out_va;
    io->out_frm.phyaddr[0] = out_pa;
    io->crp_window.w = PROBE_W;
    io->crp_window.h = PROBE_H;
    io->ife_strp_num = 1;
    io->ife_strp[0] = PROBE_W;
    io->inte_en = KDRV_IFE_INTERRUPT_FMD | KDRV_IFE_INTERRUPT_FRAME_ERR |
        KDRV_IFE_INTERRUPT_BUFOVFL | KDRV_IFE_INTERRUPT_DEC_ERR1 |
        KDRV_IFE_INTERRUPT_DEC_ERR2 | KDRV_IFE_INTERRUPT_DEC_ERR3;
    /* Standard RAW D2D, not thermal_mode: that mode requires F-curve.
     * Start with all processing disabled so precision loss cannot hide in it. */
    io->thermal_mode_en = 0;
    iq->update = KDRV_IFE_UPDATE_ALL;
    iq->filt = probe_sdk_filter;
    iq->filt.enable = nr ? 1 : 0;
    iq->filt.motion_set.motion_en = 0; /* no BNR motion-map input */
    iq->filt.rbfill.enable = 0;        /* monochrome, not RGB-IR */
}
#endif
