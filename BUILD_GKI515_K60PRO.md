# K60 Pro · GKI 5.15 编译说明

## 1. 目标机

| 项 | 值 |
|----|----|
| 设备 | Redmi K60 Pro |
| `uname -r` | `5.15.189-android13-8-00012-g742fd6fce289-ab14327796` |
| Google GKI 分支 | `android13-5.15` |
| 对齐 commit | `742fd6fce289` |
| 源码头文件 | `MY_LINUX_VERSION_CODE KERNEL_VERSION(5,15,189)` |

换机 / 换内核：**先改两个 `ver_control.h` 的版本宏**，再按该机是否 CFI/LTO 调整 workflow。

---

## 2. 一键编译（推荐）

仓库：https://github.com/ccskzsn502/rwProcMem33  

1. Actions → **Build GKI 5.15 Modules (K60 Pro)**  
2. `workflow_dispatch` 手动跑，或 push 到 `master` 自动跑  
3. 下载：
   - **`k60pro-gki515-ko`**：两个 `.ko` + `BUILD_INFO.txt`（已打 PAC landing）
   - **`k60pro-userspace-arm64`**：`testHwBpServer` 等

### CI 关键步骤（理解用）

1. 拉 Google `kernel/common` @ `742fd6fce289`
2. `gki_defconfig` + 打开：
   - `CONFIG_MODULES` / `KALLSYMS` / `PERF_EVENTS` / `HW_BREAKPOINT`
   - **`CONFIG_LTO_CLANG_THIN` + `CONFIG_CFI_CLANG`**（必须）
3. `prepare` + `modules_prepare`（必要时 in-tree `modules` 生成 `Module.symvers`）
4. **隐藏/清空 `Module.symvers`** 后 OOT 编模块（empty-CRC）
5. `python3 scripts/patch_module_cfi_landing.py --in-place *.ko`
6. 上传 artifact

工具链：Ubuntu 22.04 + **Clang 14 / LLD 14** + `aarch64-linux-gnu`。

---

## 3. 本地复现 CI（Linux）

```bash
# 依赖示例
sudo apt-get install -y build-essential bc bison flex libssl-dev libelf-dev \
  clang-14 lld-14 llvm-14 gcc-aarch64-linux-gnu

# 1) 准备 GKI 源码树（与 workflow 相同 commit）
# 2) make LLVM=1 ARCH=arm64 gki_defconfig
# 3) 打开 MODULES / CFI / ThinLTO / PERF / HW_BREAKPOINT 等（见 workflow）
# 4) make LLVM=1 ARCH=arm64 prepare modules_prepare -j$(nproc)
# 5) 备份后清空 Module.symvers
# 6) make -C $KDIR M=$PWD/hwBreakpointProcModule/hwBreakpointProc_module \
#      ARCH=arm64 LLVM=1 KBUILD_MODPOST_WARN=1 modules
# 7) 对 rwProcMem 同样执行
# 8) python3 scripts/patch_module_cfi_landing.py --in-place *.ko
```

**注意：**

- 不要用“关 CFI 的简易 defconfig”图省事，K60 Pro 上会 **insmod 重启**
- 不要跳过 PAC landing
- 不要把未 patch 的 `.ko` 和 patch 后的混用

---

## 4. 加载与验证

```bash
adb push *.ko /data/local/tmp/
adb shell su -c "insmod /data/local/tmp/rwProcMem_module.ko"
adb shell su -c "insmod /data/local/tmp/hwBreakpointProc_module.ko"
adb shell su -c "dmesg | tail -50"
```

成功迹象（示意）：

- `dmesg` 有模块 init 日志（`CONFIG_DEBUG_PRINTK` 打开时更明显）
- 无 `module_layout` 版本冲突
- 无立即重启

Server：

```bash
adb push testHwBpServer /data/local/tmp/
adb shell su -c "chmod 755 /data/local/tmp/testHwBpServer"
adb shell su -c "/data/local/tmp/testHwBpServer"
adb forward tcp:3170 tcp:3170
```

Auth key 与 `ver_control.h` 中 `CONFIG_PROC_NODE_AUTH_KEY` 一致。

---

## 5. HitItem 协议（必须一致）

| 字段 | 说明 |
|------|------|
| 总大小 | **456** 字节（pack(1)） |
| `hit_addr` | X=PC；R/W=数据地址 |
| `regs` | 命中时寄存器（含真实 PC） |
| `bp_addr` | 断点地址 |
| `hit_type` | 断点类型 |
| `stack_count` + `stack_pcs[16]` | FP 栈 |

旧协议若仍按 **312** 解析，会出现“PC 等于断点地址、栈很少/错乱”等假象。  
改内核后务必重编 **server + 桌面 UI**。

---

## 6. 常见坑

1. **`module_layout` CRC 不一致**  
   本 CI 用 empty-CRC 规避；若你改回“带真实 CRC”，必须使用与机内完全一致的 `Module.symvers`。

2. **insmod 重启**  
   优先查：CFI/LTO 是否打开、是否 PAC landing、是否用了错误内核树。

3. **hide / 反 ptrace / modify-hit-next**  
   默认关闭。CFI 上这些路径曾导致延迟重启或崩。

4. **反复 rmmod**  
   有 hide 残留风险时，**重启**比硬 rmmod 更安全。

5. **二进制进仓库**  
   禁止。只用 Actions artifact 或本地 `out/`（gitignored）。

---

## 7. 文件索引

| 路径 | 用途 |
|------|------|
| `hwBreakpointProcModule/hwBreakpointProc_module/` | 硬件断点内核源码 |
| `rwProcMem33Module/rwProcMem_module/` | 读写内核源码 |
| `*/ver_control.h` | 版本宏、auth key、功能开关 |
| `scripts/patch_module_cfi_landing.py` | `__cfi_check` PAC 补丁 |
| `.github/workflows/build-gki515.yml` | 官方推荐编译流水线 |

更完整的项目说明见根目录 [`README.md`](README.md)。
