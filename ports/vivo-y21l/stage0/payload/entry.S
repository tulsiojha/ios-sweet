/*
 * stage0 v5 AArch64 entry. The QSEE monitor erets here at EL1 with
 * x0 = dtb physical address, MMU and caches off, VA = PA.
 *
 * Interrupts stay masked for the whole payload (everything is polled);
 * the vector table only exists so a fault leaves a report instead of
 * an ignored exception.
 */
	.section .text.entry, "ax", %progbits
	.global _start
_start:
	msr	daifset, #0xf
	mrs	x1, sctlr_el1
	bic	x1, x1, #(1 << 0)		// MMU off
	bic	x1, x1, #(1 << 2)		// D-cache off
	bic	x1, x1, #(1 << 12)		// I-cache off
	msr	sctlr_el1, x1
	isb

	ldr	x1, =0x8b000000			// stack top: 176 MB into DRAM
	mov	sp, x1

	ldr	x1, =vectors
	msr	vbar_el1, x1
	isb

	ldr	x1, =__bss_start
	ldr	x2, =__bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	bl	kmain				// x0 = dtb
3:	wfi
	b	3b

	.balign	2048
vectors:
	.rept	16
	b	exc_common
	.balign	128
	.endr

exc_common:
	msr	daifset, #0xf
	sub	sp, sp, #176
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	mrs	x0, esr_el1
	mrs	x1, elr_el1
	mrs	x2, far_el1
	bl	exc_report
4:	b	4b
