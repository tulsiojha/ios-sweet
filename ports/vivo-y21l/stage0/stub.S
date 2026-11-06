/*
 * stage0 v2: prove the unlocked LK executes a replaced kernel.
 *
 * Entered in AArch32 at 0x80008000 (stock boot.img kernel_addr).
 * Signal: after an 8 s wait (ARM generic timer, the same one LK uses
 * for udelay), write 0 to qcom,pshold (0x4ab000, DTS restart@4ab000)
 * which pulls PS_HOLD low and powers the PMIC off. A display that keeps
 * showing the vivo logo and then goes dark ~8 s later proves execution
 * even though the command-mode panel never shows our framebuffer fill.
 *
 * If PS_HOLD turns out not to work, fall back to filling the splash
 * region red and spinning (hold power to reset).
 *
 * Inlined, no relocations: llvm-mc + llvm-objcopy produce a flat binary.
 */
	.syntax unified
	.arch armv7-a
	.arm

	.section .text, "ax", %progbits
	.global _start
_start:
	adr	r1, msg
uart_loop:
	ldrb	r0, [r1], #1
	cmp	r0, #0
	beq	wait_start
	movw	r2, #0x00a4
	movt	r2, #0x78b0		@ UARTDM_SR
	movw	r3, #0xffff
	movt	r3, #0x00ff
poll:
	ldr	r12, [r2]
	tst	r12, #4			@ TXRDY
	bne	send
	subs	r3, r3, #1
	bne	poll
	b	wait_start
send:
	movw	r2, #0x0070
	movt	r2, #0x78b0		@ UARTDM_TF
	str	r0, [r2]
	b	uart_loop

wait_start:
	/* 8 s on CNTPCT: target = now + 8*CNTFRQ */
	mrc	p15, 0, r4, c14, c0, 0	@ CNTFRQ
	cmp	r4, #0
	movweq	r4, #0xf800
	movteq	r4, #0x0124		@ default 19.2 MHz
	lsl	r5, r4, #3		@ delta lo = frq * 8
	lsr	r6, r4, #29		@ delta hi
	mrrc	p15, 0, r0, r1, c14	@ CNTPCT
	adds	r0, r0, r5
	adc	r1, r1, r6		@ r1:r0 = target
	movw	r7, #0xffff
	movt	r7, #0x0fff		@ poll budget, in case the counter is dead
wait_loop:
	mrrc	p15, 0, r2, r3, c14
	subs	r12, r2, r0
	sbcs	r12, r3, r1
	bpl	poweroff		@ now >= target
	subs	r7, r7, #1
	bne	wait_loop

poweroff:
	/* deassert PS_HOLD: PMIC powers the phone off */
	movw	r0, #0xb000
	movt	r0, #0x004a
	mov	r1, #0
	str	r1, [r0]
	dsb

	/* PS_HOLD did not take us down: make something visible instead */
	movw	r0, #0x0000
	movt	r0, #0x8300
	movw	r3, #0x0000
	movt	r3, #0x8440		@ 0x83000000 + 0x1400000
	movw	r2, #0x0000
	movt	r2, #0xffff
1:	str	r2, [r0], #4
	cmp	r0, r3
	blo	1b
	movw	r0, #0x0000
	movt	r0, #0x8300
2:	mcr	p15, 0, r0, c7, c10, 1	@ clean by MVA, 64B lines
	add	r0, r0, #64
	cmp	r0, r3
	blo	2b
	dsb

3:	b	3b

msg:
	.asciz	"\n\nVIVO STAGE0 v2: ARM32 KERNEL RUNNING\n\n"
