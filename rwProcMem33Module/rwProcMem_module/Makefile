obj-m += rwProcMem_module.o
ccflags-y += -fno-stack-protector
ccflags-y += -Wno-error
# Keep mild lsnbm flags only. Do NOT disable CFI sanitize here: on CFI kernels
# the loader calls init via CFI jump tables and expects module.__cfi_check.
# Runtime still patches __cfi_slowpath_diag (lsnbm-style) for type-hash mismatch.
