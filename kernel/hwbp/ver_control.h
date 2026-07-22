#ifndef VER_CONTROL_H_
#define VER_CONTROL_H_
#include <linux/version.h>

// 独立内核模块入口模式
#define CONFIG_MODULE_GUIDE_ENTRY

// 生成proc用户层交互节点文件
#define CONFIG_USE_PROC_FILE_NODE
// 隐蔽通信密钥
#define CONFIG_PROC_NODE_AUTH_KEY "dce3771681d4c7a143d5d06b7d32548e"

// 调试打印（排查加载/死机时打开；生产热路径默认关闭）
// #define CONFIG_DEBUG_PRINTK

// kprobe 隐藏 /proc 目录在 CFI 内核上易触发崩溃，默认关闭
// #define CONFIG_HIDE_PROCFS_DIR

// 仅 printk 的最小 init（排查 CFI/ABI）。empty-CRC + PAC landing 已稳定，恢复完整断点。
// #define CONFIG_SAFE_MINIMAL_INIT

#ifndef CONFIG_SAFE_MINIMAL_INIT
// 动态寻址模式
#define CONFIG_KALLSYMS_LOOKUP_NAME

// 精准命中记录：会把数据写断点临时改成 X 断点（pc+4）。
// 在 GKI 5.15 + CFI 上，该路径命中后卸载/release 易延迟重启，先关闭。
// #define CONFIG_MODIFY_HIT_NEXT_MODE

// 反PTRACE：kretprobe hook arch_ptrace，CFI 内核上同样偏险，先关闭。
// #define CONFIG_ANTI_PTRACE_DETECTION_MODE
#endif

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

#endif /* VER_CONTROL_H_ */

