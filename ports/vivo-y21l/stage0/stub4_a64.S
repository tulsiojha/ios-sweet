/*
 * stage0 v4 (AArch64 part): runs at EL1 after the secure monitor's 32->64
 * hand-off, entered at 0x8000a000 with x0 = dtb physical address.
 * Deasserts PS_HOLD so a successful hand-off is visible as a power-off.
 */
	.arch armv8-a

	.section .text, "ax", %progbits
	.global _start
_start:
	movz	x1, #0xb000
	movk	x1, #0x4a, lsl #16		// x1 = 0x4ab000 (qcom,pshold)
	str	wzr, [x1]
	dsb	sy
1:	b	1b
