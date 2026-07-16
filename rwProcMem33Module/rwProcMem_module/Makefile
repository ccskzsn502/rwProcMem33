obj-m += rwProcMem_module.o
ccflags-y += -fno-stack-protector
ccflags-y += -Wno-error
# lsnbm-style: avoid SCS/PAC mismatch traps on some GKI builds (keep mild)
ccflags-y += -fno-sanitize=cfi
