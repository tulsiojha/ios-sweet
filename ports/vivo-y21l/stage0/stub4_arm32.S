/*
 * stage0 v4 (ARM32 part): hand off to AArch64 through the secure monitor.
 *
 * Entered at 0x80008000 in AArch32 at EL2 (LK's 32-bit kernel entry, r2 =
 * tags/dtb physical address). Replicates the monitor call that this device's
 * own LK uses for 64-bit kernels (scm_elexec_call): an el1_system_param with
 * el1_x0 = dtb and el1_elr = 0x8000a000, sent first with the ARM SCM (SIP)
 * encoding and then, if that returns, with the legacy QSEE encoding.
 *
 * On success the monitor erets to the AArch64 payload at 0x8000a000, which
 * deasserts PS_HOLD -> the phone powers off (boot loop while USB is attached).
 * On failure this stub spins: the vivo logo stays on screen forever.
 *
 * Cache maintenance is inlined (no bl): llvm-mc cannot resolve a call without
 * a linker and a flat binary keeps the placeholder branch-to-self.
 */
	.syntax unified
	.arch armv7-a
	.arm

	.equ PARAM,  0x80009000
	.equ CMDBUF, 0x80009400
	.equ CTX,    0x80009480

	.section .text, "ax", %progbits
	.global _start
_start:
	cpsid	if
	mov	r10, r2			@ r2 = dtb/tags physical, from LK

	@ build el1_system_param (10 x u64, elr at +0x48)
	ldr	r0, =PARAM
	mov	r1, #0
	mov	r2, #20
1:	str	r1, [r0], #4
	subs	r2, r2, #1
	bne	1b
	ldr	r0, =PARAM
	str	r10, [r0]		@ el1_x0  = dtb offset
	ldr	r1, =0x8000a000
	str	r1, [r0, #0x48]		@ el1_elr = AArch64 payload

	@ make the param visible to the secure world
	ldr	r0, =PARAM
	mov	r1, #0x1000
	add	r1, r1, r0
	bic	r0, r0, #63
2:	mcr	p15, 0, r0, c7, c14, 1	@ DCCIMVAC
	add	r0, r0, #64
	cmp	r0, r1
	blo	2b
	dsb	sy
	isb	sy

	@ attempt 1: ARM SCM (SIP) encoding of the milestone 32->64 call
	ldr	r0, =0x0200010f		@ MAKE_SIP_SCM_CMD(1, 0xf)
	mov	r1, #0x12		@ 2 args, first is a read buffer
	ldr	r2, =PARAM
	mov	r3, #0x50		@ sizeof(el1_system_param)
	mov	r4, #0
	mov	r5, #0
	mov	r6, #0
	.inst	0xe1600070		@ smc #0

	@ attempt 2: legacy QSEE encoding; returns only on failure
	ldr	r4, =CMDBUF
	ldr	r1, =104		@ 16 hdr + 8 resp hdr + 80 param
	str	r1, [r4]
	mov	r1, #16
	str	r1, [r4, #4]		@ buf_offset
	mov	r1, #96
	str	r1, [r4, #8]		@ resp_hdr_offset
	ldr	r1, =0x40f		@ (svc 1 << 10) | cmd 0xf
	str	r1, [r4, #12]

	ldr	r2, =PARAM
	add	r0, r4, #16
	mov	r3, #20
3:	ldr	r1, [r2], #4
	str	r1, [r0], #4
	subs	r3, r3, #1
	bne	3b

	ldr	r0, =CMDBUF
	mov	r1, #0x200
	add	r1, r1, r0
	bic	r0, r0, #63
4:	mcr	p15, 0, r0, c7, c14, 1	@ DCCIMVAC
	add	r0, r0, #64
	cmp	r0, r1
	blo	4b
	dsb	sy
	isb	sy

	mov	r0, #1			@ SCM_SMC32_CALL
	ldr	r1, =CTX
	ldr	r2, =CMDBUF
	.inst	0xe1600070		@ smc #0

5:	b	5b			@ both conventions refused: stuck at logo
