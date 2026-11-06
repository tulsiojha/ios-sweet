OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS
{
	. = 0x8000a000;
	.text : {
		*(.text.entry)
		*(.text .text.*)
	}
	.rodata : {
		*(.rodata .rodata.*)
	}
	.data : {
		*(.data .data.*)
	}
	. = ALIGN(4096);
	__bss_start = .;
	.bss (NOLOAD) : {
		*(.bss .bss.*)
		*(COMMON)
	}
	. = ALIGN(16);
	__bss_end = .;
	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
		*(.ARM.exidx*)
	}
}
