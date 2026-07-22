// 远端进程只读页映射接口
#ifndef KERNEL_OOK_MIRROR_H
#define KERNEL_OOK_MIRROR_H

#include <linux/mm.h>
#include <linux/types.h>
#include <linux/pid.h>
#include <linux/uaccess.h>

struct task_struct;

#define MIRROR_STATUS_MAGIC   0x4f4f4b4dU
#define MIRROR_STATUS_VERSION 1U
#define MIRROR_STATE_ACTIVE   1U
#define MIRROR_STATE_INVALID  2U
#define MIRROR_MAX_SIZE       (64U * 1024U)

struct mirror_request {
    uint32_t pid;
    uint32_t flags;
    uint64_t remote_addr;
    uint64_t length;
};

struct mirror_status {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t reserved;
    uint64_t remote_addr;
    uint64_t length;
    uint64_t generation;
};

int mirror_create_fd(pid_t pid, unsigned long remote_addr,
                     unsigned long length);
/*
 * One-shot pin + kernel->user copy for arbitrary ranges.
 * Returns bytes copied (>=0) or -errno.
 */
ssize_t mirror_copy_from_remote(struct pid *pid_struct,
                                unsigned long remote_addr,
                                char __user *ubuf, size_t size,
                                bool force);
/*
 * One-shot pin + user->remote present pages.
 * Returns bytes written (>=0) or -errno.
 */
ssize_t mirror_copy_to_remote(struct pid *pid_struct,
                              unsigned long remote_addr,
                              const char __user *ubuf, size_t size,
                              bool force);
/* Kernel-buffer variant for in-kernel scanners (no usercopy). */
ssize_t mirror_copy_from_remote_kbuf(struct pid *pid_struct,
                                     unsigned long remote_addr,
                                     void *kbuf, size_t size,
                                     bool force);
int mirror_init(void);
void mirror_exit(void);
struct task_struct *mirror_worker_task(void);

#endif
