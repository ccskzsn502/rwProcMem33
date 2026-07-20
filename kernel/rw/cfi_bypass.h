#ifndef RW_CFI_BYPASS_H_
#define RW_CFI_BYPASS_H_

/*
 * Runtime CFI bypass for GKI 5.10/5.15, adapted from lsnbm/Linux-android-arm64
 * (export_fun.h bypass_cfi).
 *
 * Compile-time clean path (no post-link py):
 *  - empty-CRC OOT build (hide Module.symvers + KBUILD_MODPOST_WARN=1)
 *  - Makefile: strip CC_FLAGS_CFI + -fno-sanitize=cfi + -fno-lto
 *  - strong asm landing: kernel/cfi_landing.S (__cfi_check / __cfi_check_fail)
 *  - this init-time patch of kernel __cfi_slowpath / __cfi_slowpath_diag → RET
 *
 * Then: compile → insmod directly. Residual weak __cfi_check.NN must not exist.
 */
#include <linux/kprobes.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/printk.h>

#ifndef AARCH64_RET_INSTR
#define AARCH64_RET_INSTR 0xD65F03C0
#endif

__attribute__((no_sanitize("cfi")))
static unsigned long rw_kallsyms_lookup_name(const char *name)
{
	typedef unsigned long (*kallsyms_fn_t)(const char *n);
	kallsyms_fn_t fn = NULL;
	struct kprobe kp;

	memset(&kp, 0, sizeof(kp));
	kp.symbol_name = "kallsyms_lookup_name";
	if (register_kprobe(&kp) < 0)
		return 0;
	fn = (kallsyms_fn_t)kp.addr;
	unregister_kprobe(&kp);
	if (!fn)
		return 0;
	return fn(name);
}

__attribute__((no_sanitize("cfi")))
static bool rw_bypass_cfi(void)
{
	typedef int (*patch_text_fn_t)(void *addrs[], u32 insns[], int cnt);
	static bool done;
	patch_text_fn_t patch_text;
	unsigned long cfi_addr = 0;
	void *patch_addrs[1];
	u32 patch_insns[1] = { AARCH64_RET_INSTR };
	int rc;

	if (done)
		return true;

	patch_text = (patch_text_fn_t)rw_kallsyms_lookup_name("aarch64_insn_patch_text");
	if (!patch_text) {
		printk(KERN_EMERG "rwProcMem: aarch64_insn_patch_text not found\n");
		return false;
	}

	/* Match lsnbm order: 5.10 slowpath first, then 5.15 diag. */
		cfi_addr = rw_kallsyms_lookup_name("__cfi_slowpath"); /* 5.10 */
		if (!cfi_addr)
			cfi_addr = rw_kallsyms_lookup_name("__cfi_slowpath_diag"); /* 5.15 */
		if (!cfi_addr)
			cfi_addr = rw_kallsyms_lookup_name("_cfi_slowpath"); /* 5.4 */

	if (!cfi_addr) {
		/* KP may already bypass kCFI; old slowpath may be absent. */
		printk(KERN_EMERG "rwProcMem: no CFI slowpath symbol, skip patch\n");
		done = true;
		return true;
	}

	patch_addrs[0] = (void *)cfi_addr;
	rc = patch_text(patch_addrs, patch_insns, 1);
	if (rc != 0) {
		printk(KERN_EMERG "rwProcMem: CFI slowpath patch failed rc=%d\n", rc);
		return false;
	}

	printk(KERN_EMERG "rwProcMem: CFI slowpath patched @0x%lx\n", cfi_addr);
	done = true;
	return true;
}

#endif /* RW_CFI_BYPASS_H_ */
