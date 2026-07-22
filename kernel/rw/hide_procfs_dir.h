#ifndef _HIDE_PROCFS_DIR_H_
#define _HIDE_PROCFS_DIR_H_

#include "ver_control.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/spinlock.h>

static char g_hide_dir_name[256] = {0};
static size_t g_hide_dir_name_len = 0;
static bool g_hide_procfs_registered = false;
/*
 * Concurrent readdir can nest. Keep a small stack of previous actors
 * instead of a single global, under a spinlock.
 */
#define HIDE_FILLDIR_STACK 8
static filldir_t g_old_filldir_stack[HIDE_FILLDIR_STACK];
static int g_old_filldir_sp = 0;
static DEFINE_SPINLOCK(g_hide_filldir_lock);

#if MY_LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
static int my_filldir(struct dir_context *buf,
                      const char *name,
                      int namelen,
                      loff_t offset,
                      u64 ino,
                      unsigned int d_type)
{
	filldir_t prev;
	unsigned long flags;

	if (namelen == (int)g_hide_dir_name_len &&
	    !strncmp(name, g_hide_dir_name, namelen)) {
		return 0;
	}
	spin_lock_irqsave(&g_hide_filldir_lock, flags);
	prev = (g_old_filldir_sp > 0) ?
		g_old_filldir_stack[g_old_filldir_sp - 1] : NULL;
	spin_unlock_irqrestore(&g_hide_filldir_lock, flags);
	if (!prev)
		return 0;
	return prev(buf, name, namelen, offset, ino, d_type);
}
#else
static bool my_filldir(struct dir_context *ctx,
                       const char *name,
                       int namelen,
                       loff_t offset,
                       u64 ino,
                       unsigned int d_type)
{
	filldir_t prev;
	unsigned long flags;

	if (namelen == (int)g_hide_dir_name_len &&
	    !strncmp(name, g_hide_dir_name, namelen)) {
		return true;
	}
	spin_lock_irqsave(&g_hide_filldir_lock, flags);
	prev = (g_old_filldir_sp > 0) ?
		g_old_filldir_stack[g_old_filldir_sp - 1] : NULL;
	spin_unlock_irqrestore(&g_hide_filldir_lock, flags);
	if (!prev)
		return true;
	return prev(ctx, name, namelen, offset, ino, d_type);
}
#endif

static int handler_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct dir_context *ctx = (struct dir_context *)regs->regs[1];
	unsigned long flags;

	if (!ctx)
		return 0;
	spin_lock_irqsave(&g_hide_filldir_lock, flags);
	if (g_old_filldir_sp < HIDE_FILLDIR_STACK) {
		g_old_filldir_stack[g_old_filldir_sp++] = ctx->actor;
		ctx->actor = my_filldir;
	}
	spin_unlock_irqrestore(&g_hide_filldir_lock, flags);
	return 0;
}

/* Best-effort pop when readdir returns; kprobe post may not always fire. */
static void handler_post(struct kprobe *kp, struct pt_regs *regs,
			 unsigned long flags)
{
	unsigned long irqf;
	spin_lock_irqsave(&g_hide_filldir_lock, irqf);
	if (g_old_filldir_sp > 0)
		g_old_filldir_sp--;
	spin_unlock_irqrestore(&g_hide_filldir_lock, irqf);
}

static struct kprobe kp_hide_procfs_dir = {
	.symbol_name = "proc_root_readdir",
	.pre_handler = handler_pre,
	.post_handler = handler_post,
};

static bool start_hide_procfs_dir(const char* hide_dir_name)
{
	int ret;
	strlcpy(g_hide_dir_name, hide_dir_name, sizeof(g_hide_dir_name));
	g_hide_dir_name_len = strlen(g_hide_dir_name);
	ret = register_kprobe(&kp_hide_procfs_dir);
	if (ret) {
		printk_debug("[hide_procfs_dir] register_kprobe failed: %d\n", ret);
		g_hide_procfs_registered = false;
		return false;
	}
	g_hide_procfs_registered = true;
	printk_debug("[hide_procfs_dir] kprobe installed, hiding \"%s\"\n", g_hide_dir_name);
	return true;
}

static void stop_hide_procfs_dir(void)
{
	if (g_hide_procfs_registered) {
		unregister_kprobe(&kp_hide_procfs_dir);
		g_hide_procfs_registered = false;
		printk_debug("[hide_procfs_dir] kprobe removed\n");
	}
}

#endif  // _HIDE_PROCFS_DIR_H_
