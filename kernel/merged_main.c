/*
 * Single-node merged module: R/W + kernel search + HWBP on one /proc auth key.
 * CMDs: RW 1..14, HWBP 100..111. Auth key from CONFIG_PROC_NODE_AUTH_KEY (RW key).
 *
 * Subsystems compile in rw_core.o / hwbp_core.o with CONFIG_MERGED_MODULE.
 * This file only owns init_module, CFI bypass, and the single /proc node.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/version.h>

#ifndef CONFIG_MERGED_MODULE
#define CONFIG_MERGED_MODULE 1
#endif

/* Shared single-node key + version macros (RW). */
#include "rw/ver_control.h"
#include "rw/cfi_bypass.h"
#include "rw/api_proxy.h"
#ifdef CONFIG_HIDE_PROCFS_DIR
#include "rw/hide_procfs_dir.h"
#endif

/* ioctl_request + subsystem entry points (no full static state here). */
#ifndef IOCTL_REQUEST_DEFINED
#define IOCTL_REQUEST_DEFINED
#pragma pack(push,1)
struct ioctl_request {
	char     cmd;
	uint64_t param1;
	uint64_t param2;
	uint64_t param3;
	uint64_t buf_size;
};
#pragma pack(pop)
#endif

int rw_subsystem_init(void);
void rw_subsystem_exit(void);
ssize_t rw_dispatch_command(struct ioctl_request *hdr, char __user *buf);

int hwbp_subsystem_init(void);
void hwbp_subsystem_exit(void);
ssize_t hwbp_dispatch_command(struct ioctl_request *hdr, char __user *buf);

static struct proc_dir_entry *g_merged_proc_parent;
static struct proc_dir_entry *g_merged_proc_entry;

static ssize_t merged_read(struct file *filp, char __user *buf, size_t size, loff_t *ppos)
{
	struct ioctl_request hdr = {0};
	size_t header_size = sizeof(hdr);
	unsigned char cmd;

	if (size < header_size)
		return -EINVAL;
	if (x_copy_from_user(&hdr, buf, header_size))
		return -EFAULT;
	if (size < header_size + hdr.buf_size)
		return -EINVAL;

	cmd = (unsigned char)hdr.cmd;
	if (cmd >= 100)
		return hwbp_dispatch_command(&hdr, buf + header_size);
	return rw_dispatch_command(&hdr, buf + header_size);
}

static int merged_release(struct inode *inode, struct file *filp)
{
	/* Same policy as HWBP standalone: do not tear down BPs on fd close. */
	return 0;
}

static const struct proc_ops merged_proc_ops = {
	.proc_read = merged_read,
	.proc_release = merged_release,
};

/*
 * GKI CFI may call module __cfi_check before init runs.
 * Historical stable dual modules used post-link pad:
 *   paciasp; autiasp; ret
 * Current -fno-sanitize=cfi residual was only 8B: paciasp; brk → hard reboot.
 * Provide a real landing in source so compile→insmod needs no Python.
 */
__attribute__((naked, used, noinline, no_sanitize("cfi")))
void __cfi_check(unsigned long ignored, void *target_addr, void *diag)
{
	asm volatile(
		"paciasp\n"
		"autiasp\n"
		"ret\n"
	);
}

__attribute__((used, noinline, no_sanitize("cfi")))
void __cfi_check_fail(void *data)
{
	/* accept-all: never panic from module CFI fail path */
	(void)data;
}

static int __init merged_init(void)
{
	int ret;

	if (!rw_bypass_cfi())
		printk(KERN_EMERG "merged: CFI bypass failed (continuing)\n");

	ret = rw_subsystem_init();
	if (ret) {
		printk(KERN_EMERG "merged: rw_subsystem_init failed %d\n", ret);
		return ret;
	}

	ret = hwbp_subsystem_init();
	if (ret) {
		printk(KERN_EMERG "merged: hwbp_subsystem_init failed %d\n", ret);
		rw_subsystem_exit();
		return ret;
	}

#ifdef CONFIG_USE_PROC_FILE_NODE
	g_merged_proc_parent = proc_mkdir(CONFIG_PROC_NODE_AUTH_KEY, NULL);
	if (!g_merged_proc_parent) {
		printk(KERN_EMERG "merged: proc_mkdir failed\n");
		hwbp_subsystem_exit();
		rw_subsystem_exit();
		return -ENOMEM;
	}
	g_merged_proc_entry = proc_create(CONFIG_PROC_NODE_AUTH_KEY,
					  S_IRUGO | S_IWUGO,
					  g_merged_proc_parent,
					  &merged_proc_ops);
	if (!g_merged_proc_entry) {
		printk(KERN_EMERG "merged: proc_create failed\n");
		proc_remove(g_merged_proc_parent);
		g_merged_proc_parent = NULL;
		hwbp_subsystem_exit();
		rw_subsystem_exit();
		return -ENOMEM;
	}
#ifdef CONFIG_HIDE_PROCFS_DIR
	if (!start_hide_procfs_dir(CONFIG_PROC_NODE_AUTH_KEY))
		printk(KERN_EMERG "merged: hide_procfs_dir failed, continue\n");
#endif
	printk(KERN_EMERG "merged: single node /proc/%s/%s (RW 1-14, HWBP 100+)\n",
	       CONFIG_PROC_NODE_AUTH_KEY, CONFIG_PROC_NODE_AUTH_KEY);
#endif
	return 0;
}

static void __exit merged_exit(void)
{
#ifdef CONFIG_USE_PROC_FILE_NODE
	if (g_merged_proc_entry) {
		proc_remove(g_merged_proc_entry);
		g_merged_proc_entry = NULL;
	}
	if (g_merged_proc_parent) {
		proc_remove(g_merged_proc_parent);
		g_merged_proc_parent = NULL;
	}
#ifdef CONFIG_HIDE_PROCFS_DIR
	stop_hide_procfs_dir();
#endif
#endif
	hwbp_subsystem_exit();
	rw_subsystem_exit();
	printk(KERN_EMERG "merged: exit\n");
}

int __init init_module(void)
{
	return merged_init();
}

void __exit cleanup_module(void)
{
	merged_exit();
}

extern unsigned long __stack_chk_guard __attribute__((weak));

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux");
MODULE_DESCRIPTION("Merged RW+search+HWBP single-node module");
