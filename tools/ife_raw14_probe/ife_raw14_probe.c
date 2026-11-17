// SPDX-License-Identifier: GPL-2.0
/* Standalone NT98539A diagnostic. Stop every VideoProc user before loading. */
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <kwrap/mem.h>
#include <kdrv_type.h>
#include "probe_config.h"

static bool exclusive;
module_param(exclusive, bool, 0400);
MODULE_PARM_DESC(exclusive, "Set only after stopping all ISP/VideoProc applications");
static struct vos_mem_info_t input_mem, output_mem;
static VOS_MEM_HDL input_hdl, output_hdl;
static KDRV_IFE_IO_CFG io;
static KDRV_IFE_IQ_CFG iq;
static KDRV_IFE_JOB_CFG job = { .p_iocfg = &io, .p_iqcfg = &iq };
static DEFINE_MUTEX(probe_lock);
static DECLARE_COMPLETION(frame_done);
static struct proc_dir_entry *root;
static u32 irq_status, mismatches;
static unsigned int bypass_mask, last_pattern, last_nr;
static int last_ret = -ENODATA;
static bool output_valid, poisoned;
static unsigned int binary_readers;
static u64 elapsed_ns;

static u32 device_id(void)
{
    return KDRV_DEV_ID(KDRV_CHIP0, KDRV_VIDEOPROCS_IFE_ENGINE0, 0);
}

static INT32 frame_irq(UINT32 chip, UINT32 engine, UINT32 status,
    void *data, void *reserved)
{
    (void)chip; (void)engine; (void)data; (void)reserved;
    /* One frame, one terminal IRQ. Other notifications must not release DDR. */
    if (status & io.inte_en) {
        WRITE_ONCE(irq_status, READ_ONCE(irq_status) | status);
        complete(&frame_done);
    }
    return 0;
}

static int run_frame(unsigned int pattern, int nr)
{
    unsigned short *in = (void *)input_mem.vaddr;
    unsigned short *out = (void *)output_mem.vaddr;
    unsigned int i;
    int ret, reset_ret;
    u64 started;
    bool opened = false, submitted = false;
    output_valid = false;
    if (poisoned) { last_ret = -EIO; return -EIO; }
    if (nr && bypass_mask != 3U) { last_ret = -EPERM; return -EPERM; }
    mismatches = 0;
    last_pattern = pattern;
    last_nr = nr;
    for (i = 0; i < PROBE_PIXELS; ++i) in[i] = probe_pixel(i, pattern);
    memset(out, 0xa5, PROBE_BYTES);
    /* VOS_MEM_TYPE_NONCACHE uses coherent DMA allocation in this SDK. */
    wmb();
    probe_configure(&io, &iq, input_mem.vaddr, input_mem.paddr,
        output_mem.vaddr, output_mem.paddr, nr);
    irq_status = 0;
    reinit_completion(&frame_done);
    ret = kdrv_ife_open(KDRV_CHIP0, KDRV_VIDEOPROCS_IFE_ENGINE0);
    if (ret) goto done;
    opened = true;
    ret = kdrv_ife_set(device_id(), KDRV_IFE_PARAM_SET_CB, (void *)frame_irq);
    if (ret) goto close;
    ret = kdrv_ife_set(device_id(), KDRV_IFE_PARAM_CFG_PROCESS_CPU, &job);
    if (ret) goto close;
    started = ktime_get_ns();
    /* Treat even a failed trigger as potentially submitted until reset. */
    submitted = true;
    ret = kdrv_ife_set(device_id(), KDRV_IFE_PARAM_TRIG_SINGLE, NULL);
    if (!ret && !wait_for_completion_timeout(&frame_done, msecs_to_jiffies(2000)))
        ret = -ETIMEDOUT;
    elapsed_ns = ktime_get_ns() - started;
    if (!ret && (!(irq_status & KDRV_IFE_INTERRUPT_FMD) ||
        (irq_status & (io.inte_en & ~KDRV_IFE_INTERRUPT_FMD)))) ret = -EIO;
    if (!ret) {
        rmb();
        for (i = 0; i < PROBE_PIXELS; ++i) mismatches += in[i] != out[i];
        output_valid = true;
        if (!nr && !mismatches && pattern < 2) bypass_mask |= 1U << pattern;
        if (!nr && mismatches) bypass_mask = 0;
    }
close:
    if (ret && submitted) {
        kdrv_ife_set(device_id(), KDRV_IFE_PARAM_STOP_SINGLE, NULL);
        reset_ret = kdrv_ife_set(device_id(), KDRV_IFE_PARAM_HARD_RESET, NULL);
        if (reset_ret) {
            /* Do not release DMA buffers or unload code while DMA may run. */
            poisoned = true;
            __module_get(THIS_MODULE);
            pr_err("ife_raw14_probe: reset failed; buffers pinned until reboot\n");
            goto done;
        }
    }
    kdrv_ife_set(device_id(), KDRV_IFE_PARAM_SET_CB, NULL);
    if (opened) {
        int close_ret = kdrv_ife_close(KDRV_CHIP0, KDRV_VIDEOPROCS_IFE_ENGINE0);
        if (!ret && close_ret) { ret = close_ret; output_valid = false; }
    }
done:
    if (ret && !nr) bypass_mask = 0;
    last_ret = ret;
    pr_info("ife_raw14_probe: nr=%u pattern=%u ret=%d irq=0x%x mismatch=%u bypass_mask=%u\n",
        last_nr, last_pattern, ret, irq_status, mismatches, bypass_mask);
    return ret ? -EIO : 0;
}

static int status_show(struct seq_file *s, void *v)
{
    (void)v;
    mutex_lock(&probe_lock);
    seq_printf(s, "width=%u\nheight=%u\nformat=RAW16_LSB_RGGB_R\nthermal=0\nfcurve=0\n",
        PROBE_W, PROBE_H);
    seq_printf(s, "ret=%d\noutput_valid=%u\nnr=%u\npattern=%u\nirq=0x%x\nmismatches=%u\nbypass_mask=%u\npoisoned=%u\nelapsed_ns=%llu\n",
        last_ret, output_valid, last_nr, last_pattern, irq_status, mismatches,
        bypass_mask, poisoned, (unsigned long long)elapsed_ns);
    mutex_unlock(&probe_lock);
    return 0;
}

static int status_open(struct inode *inode, struct file *file)
{
    int ret;
    if (!try_module_get(THIS_MODULE)) return -ENODEV;
    ret = single_open(file, status_show, NULL);
    if (ret) module_put(THIS_MODULE);
    return ret;
}
static int status_release(struct inode *inode, struct file *file)
{
    int ret = single_release(inode, file);
    module_put(THIS_MODULE);
    return ret;
}
static ssize_t command_write(struct file *file, const char __user *buffer,
    size_t count, loff_t *offset)
{
    char text[32], command[16], extra;
    unsigned int pattern;
    int ret, nr;
    (void)file; (void)offset;
    if (!count || count >= sizeof(text)) return -EINVAL;
    if (copy_from_user(text, buffer, count)) return -EFAULT;
    text[count] = 0;
    if (sscanf(text, "%15s %u %c", command, &pattern, &extra) != 2 || pattern > 2)
        return -EINVAL;
    if (!strcmp(command, "bypass")) nr = 0;
    else if (!strcmp(command, "denoise")) nr = 1;
    else return -EINVAL;
    mutex_lock(&probe_lock);
    ret = binary_readers ? -EBUSY : run_frame(pattern, nr);
    mutex_unlock(&probe_lock);
    if (ret) return ret;
    return count;
}
static const struct proc_ops status_ops = {
    .proc_open = status_open, .proc_release = status_release,
    .proc_read = seq_read, .proc_lseek = seq_lseek, .proc_write = command_write,
};
static int binary_open(struct inode *inode, struct file *file)
{
    if (!try_module_get(THIS_MODULE)) return -ENODEV;
    mutex_lock(&probe_lock);
    ++binary_readers;
    file->private_data = pde_data(inode);
    mutex_unlock(&probe_lock);
    return 0;
}
static int binary_release(struct inode *inode, struct file *file)
{
    (void)inode; (void)file;
    mutex_lock(&probe_lock);
    --binary_readers;
    mutex_unlock(&probe_lock);
    module_put(THIS_MODULE);
    return 0;
}
static ssize_t binary_read(struct file *file, char __user *buffer,
    size_t count, loff_t *offset)
{
    ssize_t ret;
    mutex_lock(&probe_lock);
    ret = output_valid ? simple_read_from_buffer(buffer, count, offset,
        file->private_data, PROBE_BYTES) : -ENODATA;
    mutex_unlock(&probe_lock);
    return ret;
}
static const struct proc_ops binary_ops = {
    .proc_open = binary_open, .proc_release = binary_release,
    .proc_read = binary_read, .proc_lseek = default_llseek,
};

static int __init probe_init(void)
{
    /* kdrv_ife_open does not arbitrate ownership against kflow_ipp. */
    if (!exclusive) return -EBUSY;
    if (vos_mem_init_info(&input_mem, VOS_MEM_TYPE_NONCACHE, PROBE_BYTES, NULL) ||
        vos_mem_init_info(&output_mem, VOS_MEM_TYPE_NONCACHE, PROBE_BYTES, NULL))
        return -ENOMEM;
    input_hdl = vos_mem_alloc_contiguous(&input_mem);
    output_hdl = vos_mem_alloc_contiguous(&output_mem);
    if (!input_hdl || !output_hdl) goto fail;
    root = proc_mkdir("ife_raw14_probe", NULL);
    if (!root || !proc_create("status", 0600, root, &status_ops) ||
        !proc_create_data("input.raw", 0400, root, &binary_ops, (void *)input_mem.vaddr) ||
        !proc_create_data("output.raw", 0400, root, &binary_ops, (void *)output_mem.vaddr))
        goto fail;
    return 0;
fail:
    if (root) proc_remove(root);
    if (input_hdl) vos_mem_release_contiguous(input_hdl);
    if (output_hdl) vos_mem_release_contiguous(output_hdl);
    return -ENOMEM;
}
static void __exit probe_exit(void)
{
    proc_remove(root);
    vos_mem_release_contiguous(input_hdl);
    vos_mem_release_contiguous(output_hdl);
}
module_init(probe_init);
module_exit(probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NT98539A standalone RAW14-in-RAW16 IFE precision probe");
