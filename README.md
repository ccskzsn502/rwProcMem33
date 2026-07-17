# rwProcMem33 / hwBreakpointProc

Linux ARM64 内核模块工程（进程内存读写 + 硬件断点）。  
本仓库当前以 **Redmi K60 Pro · GKI 5.15.189** 可加载版本为主线。

> **仅供学习研究。** 需要解锁 BL / root。错误的内核模块可能导致重启、变砖风险，请自备救砖方案。

---

## 模块说明

| 目录 | 说明 |
|------|------|
| `rwProcMem33Module/rwProcMem_module` | 进程内存读写驱动（Open/Read/Write/Maps/PidList 等） |
| `hwBreakpointProcModule/hwBreakpointProc_module` | 硬件断点驱动（X/R/W/RW、命中记录、调用栈） |
| `hwBreakpointProcModule/testHwBpServer` | 手机端 TCP 服务（默认端口 **3170**） |
| `hwBreakpointProcModule/testHwBp` | 用户态调用 demo |
| `scripts/patch_module_cfi_landing.py` | 编译后给 `.ko` 打 **PAC landing**（本机必做） |
| `.github/workflows/build-gki515.yml` | GitHub Actions 一键编译 |

---

## 当前主线特性（硬件断点）

1. **CE 风格命中地址**
   - 执行断点 **X**：`hit_addr = PC`
   - 数据断点 **R / W / RW**：`hit_addr = FAR_EL1`（无效则回退 `bp_addr`）
   - 寄存器里的 `regs.pc` 仍是**访问指令 PC**，不要和 `hit_addr` 混为一谈
2. **FP 调用栈**：最多 **16** 帧（`stack_count` + `stack_pcs[]`）
3. **HitItem 大小固定 456 字节**（`#pragma pack(1)`）  
   内核 / `testHwBpServer` / 桌面 UI 协议必须一致，否则命中数据会错位
4. **GKI 5.15 + CFI 适配**
   - empty-CRC 外编（隐藏 `Module.symvers`）
   - 编译期开 **ThinLTO + CFI**（匹配机内 `struct module` 布局，init 约在 `+0x178`）
   - 编译后对 `__cfi_check` 打 **PAC landing**（`paciasp; autiasp; ret`）  
     **裸 `ret` 会在本机 insmod 时重启**
5. 默认关闭高风险选项：`CONFIG_HIDE_PROCFS_DIR`、`CONFIG_MODIFY_HIT_NEXT_MODE`、`CONFIG_ANTI_PTRACE_DETECTION_MODE`

---

## 目标机信息（已验证）

```
设备: Redmi K60 Pro
内核: 5.15.189-android13-8-00012-g742fd6fce289-ab14327796
GKI:  android13-5.15 @ 742fd6fce289
```

`ver_control.h` 中：

```c
#define MY_LINUX_VERSION_CODE KERNEL_VERSION(5,15,189)
```

两套模块都要一致。换机型必须改版本宏，并按该机 CFI/LTO/symvers 情况重编。

---

## 推荐编译方式：GitHub Actions

1. 打开：https://github.com/ccskzsn502/rwProcMem33/actions  
2. 工作流：**Build GKI 5.15 Modules (K60 Pro)**  
3. **Run workflow**（`master` 推送也会触发）  
4. 下载产物：
   - `k60pro-gki515-ko` → `hwBreakpointProc_module.ko` / `rwProcMem_module.ko` / `BUILD_INFO.txt`
   - `k60pro-userspace-arm64` → `testHwBpServer` 等

本地不要提交 `.ko` / `out/`（已在 `.gitignore`）。

### 用户态单独编译（NDK）

```bash
# 需要 Android NDK（CI 使用 r26d）
ndk-build -C hwBreakpointProcModule/testHwBp
ndk-build -C hwBreakpointProcModule/testHwBpServer
ndk-build -C rwProcMem33Module/testKo
```

Windows 下注意 `HwBreakpointMgr4.h` 里是 `linux/perf_event.h`（正斜杠）。

---

## 刷入 / 加载（root）

```bash
adb push hwBreakpointProc_module.ko /data/local/tmp/
adb push rwProcMem_module.ko /data/local/tmp/
adb push testHwBpServer /data/local/tmp/
adb shell su -c "chmod 755 /data/local/tmp/testHwBpServer"

# 建议先加载读写模块，再加载断点模块
adb shell su -c "insmod /data/local/tmp/rwProcMem_module.ko"
adb shell su -c "insmod /data/local/tmp/hwBreakpointProc_module.ko"
adb shell su -c "dmesg | tail -80"

# 启动服务 + 端口转发
adb shell su -c "/data/local/tmp/testHwBpServer"
adb forward tcp:3170 tcp:3170
```

### 通信密钥

`ver_control.h`：

```c
#define CONFIG_PROC_NODE_AUTH_KEY "dce3771681d4c7a143d5d06b7d32548e"
```

用户态 / UI 必须使用同一 key。改 key 后要同时重编内核模块与 server。

---

## 编译与加载注意事项（必读）

### 1. 必须匹配内核 ABI，不要乱下别人的 `.ko`

- 版本宏 `MY_LINUX_VERSION_CODE` 必须对准目标 `uname`
- GKI 镜像若开了 **CFI + LTO**，外编也必须开，否则：
  - `struct module` 偏移不对（init 不在 `0x178`）
  - 缺少 `__cfi_check` / JT → **insmod 瞬间重启**

### 2. empty-CRC 策略（本仓库 CI 默认）

- 对 Google `kernel/common` 做 `prepare` + `modules_prepare`，并生成真实 `Module.symvers` 备份
- **真正编 OOT 时把 `Module.symvers` 清空/隐藏**，配合 `KBUILD_MODPOST_WARN=1`
- 得到的是 **empty-CRC** 模块：不必再给机子打 CRC 补丁，但**不能**用“只关 CFI 的普通 defconfig”乱编

### 3. PAC landing 必须打

CI 在上传 artifact 前会执行：

```bash
python3 scripts/patch_module_cfi_landing.py --in-place *.ko
```

本机自编后**务必**同样 patch。`BUILD_INFO.txt` 里会写：

```
cfi_check_patch=paciasp;autiasp;ret
```

### 4. 加载失败常见原因

| 现象 | 可能原因 |
|------|----------|
| `disagrees about version of symbol module_layout` | CRC/版本不匹配；应用 empty-CRC 或换对齐的 `Module.symvers` |
| insmod 立刻重启 | 未开 CFI/LTO，或未打 PAC landing，或 init 布局错 |
| 能加载但断点无效 | server 未起、端口未 forward、auth key 不一致 |
| 命中数据错乱 / 栈乱 | **HitItem 仍按旧 312 字节解析**；请全链路用 **456** |
| `rmmod` 后异常 / 残留 | hide 相关路径有风险；本机默认关 hide；必要时**重启**再加载 |

### 5. 高风险宏（默认关闭，勿随意打开）

| 宏 | 说明 |
|----|------|
| `CONFIG_SAFE_MINIMAL_INIT` | 仅 printk 的最小 init，排查用 |
| `CONFIG_HIDE_PROCFS_DIR` | kprobe 藏 /proc，CFI 上易崩 |
| `CONFIG_MODIFY_HIT_NEXT_MODE` | 命中后改断点类型，release 易延迟重启 |
| `CONFIG_ANTI_PTRACE_DETECTION_MODE` | kretprobe hook ptrace，CFI 上偏险 |

### 6. 调试建议

- 需要排查加载时打开 `CONFIG_DEBUG_PRINTK`
- 不要在 hide 残留未明时反复 `rmmod`；优先重启
- 命中语义：R/W 看 `hit_addr`（数据地址），PC 看寄存器

### 7. 二进制不进 Git

- `out/`、`*.ko`、`*.o`、`*.mod*` 已 ignore
- 需要二进制：跑 Actions 下 artifact，或本机按 workflow 复现

---

## 目录结构（摘要）

```
rwProcMem33Module/
  rwProcMem_module/          # 读写内核源码
  testKo/ testCEServer/ ...  # 用户态 demo
hwBreakpointProcModule/
  hwBreakpointProc_module/   # 硬件断点内核源码
  testHwBpServer/            # TCP server
  testHwBp/ testHwBpClient/  # demo / 旧客户端
scripts/
  patch_module_cfi_landing.py
.github/workflows/
  build-gki515.yml
BUILD_GKI515_K60PRO.md       # 更细的 CI / 机型说明
```

---

## 更新记录（本主线）

- **2026-07**：CE 风格 `hit_addr` + FP 调用栈（最多 16）；HitItem **456**  
- **2026-07**：GKI 5.15 empty-CRC + ThinLTO/CFI + PAC landing，K60 Pro 可稳定 `insmod`  
- 历史能力：进程读写、maps、CEServer 对接等（见各子目录）

---

## 相关文档

- 详细 CI / 机型说明：[`BUILD_GKI515_K60PRO.md`](BUILD_GKI515_K60PRO.md)
- Actions：https://github.com/ccskzsn502/rwProcMem33/actions
