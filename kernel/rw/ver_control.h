#ifndef VERSION_CONTROL_H_
#define VERSION_CONTROL_H_

// �����ں�ģ�����ģʽ
#define CONFIG_MODULE_GUIDE_ENTRY

// 生成proc用户层交互节点文件
#define CONFIG_USE_PROC_FILE_NODE
// 隐蔽通信密钥
#define CONFIG_PROC_NODE_AUTH_KEY "e84523d7b60d5d341a7c4d1861773ecd"

// 调试打印（排查加载/死机时打开；生产热路径默认关闭）
// #define CONFIG_DEBUG_PRINTK

// kprobe 隐藏 /proc 目录在 CFI 内核上易触发崩溃，默认关闭
// #define CONFIG_HIDE_PROCFS_DIR

// 仅 printk 的最小 init（排查 CFI/ABI）。empty-CRC 已稳定加载，恢复完整 proc init。
// #define CONFIG_SAFE_MINIMAL_INIT

#include <linux/version.h>
#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
#endif
/* Follow the kernel tree you build against (GKI common). No pin. */
#ifndef MY_LINUX_VERSION_CODE
#define MY_LINUX_VERSION_CODE LINUX_VERSION_CODE
#endif

#ifdef CONFIG_DEBUG_PRINTK
#define printk_debug printk
#else
static inline void printk_debug(char *fmt, ...) {}
#endif

#endif /* VERSION_CONTROL_H_ */

