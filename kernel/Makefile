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
# kbuild re-appends CC_FLAGS_CFI after ccflags-y; remove those first.
ccflags-remove-y += $(CC_FLAGS_CFI)
ccflags-remove-y += -fsanitize=cfi
ccflags-remove-y += -fsanitize-cfi-cross-dso
ccflags-remove-y += -fvisibility=default
# Module must not be compiled with CFI instrumentation.
ccflags-y += -fno-sanitize=cfi
ccflags-y += $(call cc-option,-fno-sanitize-cfi-cross-dso)
# ThinLTO re-emits residual __cfi_check.NN traps — disable for this OOT module.
ccflags-y += -fno-lto
ccflags-y += $(call cc-option,-fno-lto=thin)
# Per-object belt-and-suspenders (order can still inject CFI into some units).
CFLAGS_merged_main.o += -fno-sanitize=cfi -fno-lto
CFLAGS_rw_core.o     += -fno-sanitize=cfi -fno-lto
CFLAGS_hwbp_core.o   += -fno-sanitize=cfi -fno-lto
# Module final link: no LTO bitcode merge of CFI stubs.
LDFLAGS_merged_module.o += -fno-lto
KBUILD_LDFLAGS_MODULE += -fno-lto

CFLAGS_merged_main.o += -I$(RW_DIR) -I$(HWBP_DIR)
CFLAGS_rw_core.o     += -I$(RW_DIR)
CFLAGS_hwbp_core.o   += -I$(HWBP_DIR)
