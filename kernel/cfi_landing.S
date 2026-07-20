/*
 * Strong GKI CFI landing for OOT merged_module.
 *
 * Clang/LTO can still emit weak residual stubs like:
 *   __cfi_check.NN: paciasp; brk #1
 * even when a C __cfi_check exists. A pure asm strong symbol plus
 * Makefile CFI/LTO stripping makes the product loadable without
 * post-link binary patch.
 *
 * Built with LLVM_IAS=1 (aarch64).
 */
	.arch armv8.3-a
	.text
	.align 2

	.global __cfi_check
	.type __cfi_check, %function
__cfi_check:
	hint #34			/* bti c */
	paciasp
	autiasp
	ret
	.size __cfi_check, .-__cfi_check

	.global __cfi_check_fail
	.type __cfi_check_fail, %function
	.hidden __cfi_check_fail
__cfi_check_fail:
	hint #34			/* bti c */
	paciasp
	autiasp
	ret
	.size __cfi_check_fail, .-__cfi_check_fail
