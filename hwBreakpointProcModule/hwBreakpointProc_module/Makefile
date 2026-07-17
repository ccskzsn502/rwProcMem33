obj-m += hwBreakpointProc_module.o
ccflags-y += -fno-stack-protector
ccflags-y += -Wno-error
# Keep CFI sanitize enabled so layout/__cfi_check match device loader.
# Post-link: scripts/patch_module_cfi_landing.py (paciasp;autiasp;ret).
# Runtime: cfi_bypass.h patches __cfi_slowpath_diag.
