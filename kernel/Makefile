# Single .ko: R/W + kernel search + HWBP, one /proc auth node.
# CMDs: RW 1..14, HWBP 100..111
#
# Load path (compile-time clean, no post-link py):
#   1) empty-CRC OOT (hide Module.symvers + KBUILD_MODPOST_WARN=1)
#   2) strip kbuild-injected CFI + disable ThinLTO for this module
#      (ThinLTO/CFI leave residual weak __cfi_check.NN = paciasp;brk)
#   3) strong asm PAC landing in cfi_landing.S
#   4) init: rw_bypass_cfi() patches kernel __cfi_slowpath → RET
# Build against any prepared GKI tree (no version pin in source).
# Example (modules_prepare outdir = $OUT):
#   mv $OUT/Module.symvers $OUT/Module.symvers.bak
#   make -C $KDIR O=$OUT M=$PWD ARCH=arm64 LLVM=1 LLVM_IAS=1 \
#        KBUILD_MODPOST_WARN=1 modules
#   mv $OUT/Module.symvers.bak $OUT/Module.symvers
# Then insmod merged_module.ko directly.

obj-m += merged_module.o

merged_module-y := \
	cfi_landing.o \
	merged_main.o \
	rw_core.o \
	hwbp_core.o

RW_DIR   := $(src)/rw
HWBP_DIR := $(src)/hwbp

# Match lsnbm/Linux-android-arm64 Makefile mild flags
ccflags-y += -fno-stack-protector
ccflags-y += -Wno-error
ccflags-y += -O3
ccflags-y += -DCONFIG_MERGED_MODULE

# --- CFI / LTO: must kill residual weak stubs at compile time ---
# kbuild re-appends CC_FLAGS_CFI / CC_FLAGS_LTO after ccflags-y; strip first.
# Do NOT put -fno-lto into LDFLAGS — ld.lld rejects it (CI failure).
ccflags-remove-y += $(CC_FLAGS_CFI)
ccflags-remove-y += $(CC_FLAGS_LTO)
ccflags-remove-y += -fsanitize=cfi
ccflags-remove-y += -fsanitize-cfi-cross-dso
ccflags-remove-y += -flto
ccflags-remove-y += -flto=thin
ccflags-remove-y += -fvisibility=default
# Prefer kbuild disable macros when the GKI tree defines them.
ccflags-y += $(DISABLE_CFI_CLANG)
ccflags-y += $(DISABLE_LTO_CLANG)
# Explicit fallbacks (Clang 14 / GKI 5.15). Compiler only — never LDFLAGS.
ccflags-y += -fno-sanitize=cfi
ccflags-y += -fno-lto
# CFLAGS_MODULE is applied to every module .o by kbuild.
CFLAGS_MODULE += -fno-sanitize=cfi -fno-lto
# Per-object: keep bitcode/LTO off so residual CFI stubs are not re-emitted.
CFLAGS_cfi_landing.o  += $(DISABLE_LTO_CLANG)
CFLAGS_merged_main.o  += -fno-sanitize=cfi -fno-lto $(DISABLE_CFI_CLANG) $(DISABLE_LTO_CLANG)
CFLAGS_rw_core.o      += -fno-sanitize=cfi -fno-lto $(DISABLE_CFI_CLANG) $(DISABLE_LTO_CLANG)
CFLAGS_hwbp_core.o    += -fno-sanitize=cfi -fno-lto $(DISABLE_CFI_CLANG) $(DISABLE_LTO_CLANG)

CFLAGS_merged_main.o += -I$(RW_DIR) -I$(HWBP_DIR)
CFLAGS_rw_core.o     += -I$(RW_DIR)
CFLAGS_hwbp_core.o   += -I$(HWBP_DIR)
