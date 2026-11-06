/*
 * stage0 v3: power-off beacon with no peripheral access except PS_HOLD.
 *
 * v1/v2 wrote to the BLSP1 UART first; if LK never enables its clocks, that
 * access hangs the AXI bus and the CPU never reaches the PS_HOLD write.
 * v3 touches nothing but qcom,pshold (0x4ab000, msm-poweroff driver uses the
 * same deassert to power the PMIC off).
 *
 * Entered in AArch32 at 0x80008000. If the phone powers off within ~1 s of
 * the vivo logo, the unlocked LK executed our replaced kernel.
 */
	.syntax unified
	.arch armv7-a
	.arm

	.section .text, "ax", %progbits
	.global _start
_start:
	movw	r0, #0xb000
	movt	r0, #0x004a
	mov	r1, #0
	str	r1, [r0]		@ qcom,pshold = 0 -> PS_HOLD low
	dsb
1:	b	1b
