#include "vpe_mirror.h"
#include "../../Pipeline/frame_lease.h"
#include "../../Pipeline/videoproc_devices.h"
#include "vendor_vpe.h"
#include <stdio.h>
#include <string.h>

/* Device 0 remains RAWALL. Vendor VPE channel 0 is independent of RAW ISP id. */
#define MIRROR_VPE_ID 0
static HD_PATH_ID control, path;
static HD_COMMON_MEM_VB_BLK output_block;
static UINT32 output_size;
static int vendor_ready, started, input_pending;
static FRAME_LEASE output_lease;

int vpe_mirror_input_pending(void) { return input_pending; }

HD_RESULT vpe_mirror_stop(void)
{
 HD_RESULT ret;
 if (output_lease.held) {
  ret = frame_lease_release(&output_lease, hd_videoproc_release_out_buf, path);
  if (ret != HD_OK) {
   frame_lease_report("VPE retained output", &output_lease);
   return ret;
  }
 }
 if (started) {
  ret = hd_videoproc_stop(path);
  if (ret != HD_OK) return ret;
  started = 0;
  input_pending = 0;
 }
 if (path) {
  ret = hd_videoproc_close(path);
  if (ret != HD_OK) return ret;
  path = 0;
 }
 if (control) {
  ret = hd_videoproc_close(control);
  if (ret != HD_OK) return ret;
  control = 0;
 }
 if (vendor_ready) {
  ret = vendor_vpe_uninit();
  if (ret != HD_OK) return ret;
  vendor_ready = 0;
 }
 if (output_block && output_block != HD_COMMON_MEM_VB_INVALID_BLK) {
  ret = hd_common_mem_release_block(output_block);
  if (ret != HD_OK) return ret;
  output_block = 0;
 }
 if (FRAME_DIAGNOSTICS_ENABLED) frame_lease_report("VPE", &output_lease);
 return HD_OK;
}

HD_RESULT vpe_mirror_start(UINT32 width, UINT32 height, UINT32 buffer_size)
{
 HD_VIDEOPROC_DEV_CONFIG cfg = {0};
 HD_VIDEOPROC_CTRL ctrl = {0};
 HD_VIDEOPROC_OUT out = {0};
 VPET_DCE_CTL_PARAM dce = {0};
 VPET_FLIP_ROT_PARAM flip = {0};
 HD_RESULT ret;
 const char *step = "vendor init";
 if (path || control || vendor_ready || output_block) return HD_ERR_NG;
 memset(&output_lease, 0, sizeof(output_lease));
 ret = vendor_vpe_init();
 if (ret != HD_OK) goto fail;
 vendor_ready = 1;
 step = "open mirror device control";
 ret = hd_videoproc_open(0, THERMAL_MIRROR_CTRL, &control);
 if (ret != HD_OK) goto fail;
 cfg.pipe = HD_VIDEOPROC_PIPE_VPE;
 cfg.isp_id = MIRROR_VPE_ID;
 cfg.in_max.dim.w = width;
 cfg.in_max.dim.h = height;
 cfg.in_max.pxlfmt = HD_VIDEO_PXLFMT_YUV420;
 cfg.in_max.frc = HD_VIDEO_FRC_RATIO(1,1);
 step = "configure VPE pipe";
 ret = hd_videoproc_set(control, HD_VIDEOPROC_PARAM_DEV_CONFIG, &cfg);
 if (ret != HD_OK) goto fail;
 ret = hd_videoproc_set(control, HD_VIDEOPROC_PARAM_CTRL, &ctrl);
 if (ret != HD_OK) goto fail;
 step = "open VPE output";
 ret = hd_videoproc_open(THERMAL_MIRROR_IN, THERMAL_MIRROR_OUT, &path);
 if (ret != HD_OK) goto fail;
 out.dim.w = width;
 out.dim.h = height;
 out.pxlfmt = HD_VIDEO_PXLFMT_YUV420;
 out.dir = HD_VIDEO_DIR_NONE;
 out.frc = HD_VIDEO_FRC_RATIO(1,1);
 out.depth = 1;
 ret = hd_videoproc_set(path, HD_VIDEOPROC_PARAM_OUT, &out);
 if (ret != HD_OK) goto fail;
 step = "disable VPE DCE";
 dce.id = MIRROR_VPE_ID;
 ret = vendor_vpe_get_cmd(VPET_ITEM_DCE_CTL_PARAM, &dce);
 if (ret != HD_OK) goto fail;
 dce.dce_ctl.enable = 0;
 ret = vendor_vpe_set_cmd(VPET_ITEM_DCE_CTL_PARAM, &dce);
 if (ret != HD_OK) goto fail;
 step = "set horizontal flip";
 flip.id = MIRROR_VPE_ID;
 ret = vendor_vpe_get_cmd(VPET_ITEM_FLIP_ROT_PARAM, &flip);
 if (ret != HD_OK) goto fail;
 flip.flip_rot.flip_rot_mode = VPE_ISP_H_FLIP_ROTATE_0;
 ret = vendor_vpe_set_cmd(VPET_ITEM_FLIP_ROT_PARAM, &flip);
 if (ret != HD_OK) goto fail;
 output_size = buffer_size;
 step = "start";
 ret = hd_videoproc_start(path);
 if (ret != HD_OK) goto fail;
 started = 1;
 printf("VPE mirror started: device=%u, isp_id=%u, %ux%u, horizontal flip\n",
  THERMAL_MIRROR_DEVICE, MIRROR_VPE_ID, width, height);
 return HD_OK;
fail:
 printf("VPE mirror %s failed=%d\n", step, ret);
 /* Caller also runs stop during failed-start cleanup. */
 return ret;
}

HD_RESULT vpe_mirror_process(HD_VIDEO_FRAME *input, HD_VIDEO_FRAME *output)
{
 HD_VIDEO_FRAME requested = {0};
 HD_RESULT ret, stop_ret;
 if (!started || input_pending || output_block) return HD_ERR_NG;
 output_block = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, output_size, DDR_ID0);
 if (!output_block || output_block == HD_COMMON_MEM_VB_INVALID_BLK) {
  output_block = 0;
  return HD_ERR_NG;
 }
 requested.sign = MAKEFOURCC('V','F','R','M');
 requested.blk = output_block;
 requested.reserved[0] = output_size;
 ret = hd_videoproc_push_in_buf(path, input, &requested, 0);
 if (ret != HD_OK) {
  if (hd_common_mem_release_block(output_block) == HD_OK) output_block = 0;
  return ret;
 }
 input_pending = 1;
 ret = frame_lease_pull(&output_lease, hd_videoproc_pull_out_buf, path, output, 1000);
 if (ret == HD_OK) {
  input_pending = 0;
  return HD_OK;
 }
 /* Never return a still-in-use source buffer to RAWALL after a timeout. */
 stop_ret = hd_videoproc_stop(path);
 if (stop_ret == HD_OK) {
  started = 0;
  input_pending = 0;
 } else {
  printf("VPE stop after pull error failed=%d; retaining source buffer\n", stop_ret);
 }
 printf("VPE mirror pull failed=%d; restart Liveview required\n", ret);
 return ret;
}

HD_RESULT vpe_mirror_release(HD_VIDEO_FRAME *output)
{
 HD_RESULT ret;
 (void)output;
 ret = frame_lease_release(&output_lease, hd_videoproc_release_out_buf, path);
 if (ret != HD_OK) return ret;
 /* Drop our reference; encoder references prevent premature pool reuse. */
 ret = hd_common_mem_release_block(output_block);
 if (ret == HD_OK) output_block = 0;
 return ret;
}
