/*
 * Throwaway "kernel" for the SWEETXNU pipeline test. loader.c jumps here
 * with the MMU off and x0 = boot_args physical address. Writing
 * qcom,pshold (0x4ab000) powers the PMIC off: a power-off right after
 * the loader log lines is the pass signal. No data references, because
 * the segment is copied to a physical address different from its
 * link-time VA.
 */
	.section __TEXT,__text,regular,pure_instructions
	.globl _start
_start:
	movz	x1, #0xb000
	movk	x1, #0x4a, lsl #16	// 0x4ab000 = qcom,pshold
	str	wzr, [x1]
1:	wfi
	b	1b