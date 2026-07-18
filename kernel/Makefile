# Single .ko: R/W + kernel search + HWBP, one /proc auth node.
# CMDs: RW 1..14, HWBP 100..111
#
# Load path (no post-link py):
#   empty-CRC + runtime CFI slowpath + source __cfi_check PAC landing
#   (paciasp;autiasp;ret in merged_main.c — same pad dual modules needed)
# Build against any prepared GKI tree (no version pin in source).
# API branches use LINUX_VERSION_CODE from that tree.
# Example (modules_prepare outdir = $OUT):
#   # hide Module.symvers so modpost writes empty __versions
#   mv $OUT/Module.symvers $OUT/Module.symvers.bak
#   make -C $KDIR O=$OUT M=$PWD ARCH=arm64 LLVM=1 LLVM_IAS=1 \
#        KBUILD_MODPOST_WARN=1 modules
#   mv $OUT/Module.symvers.bak $OUT/Module.symvers
# Then insmod merged_module.ko directly (init calls rw_bypass_cfi).

obj-m += merged_module.o

merged_module-y := \
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
# Avoid incomplete module __cfi_check so insmod can reach init without py patch
ccflags-y += -fno-sanitize=cfi

CFLAGS_merged_main.o += -I$(RW_DIR) -I$(HWBP_DIR)
CFLAGS_rw_core.o     += -I$(RW_DIR)
CFLAGS_hwbp_core.o   += -I$(HWBP_DIR)
