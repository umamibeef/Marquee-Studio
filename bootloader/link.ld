/* BOOTSTRAP V2.00 runs in place from the 512 KB boot flash on CS0. There is no writable
 * data: all state lives in DPRAM (see include/board.h), so .data/.bss must stay empty. */
OUTPUT_ARCH(powerpc)
ENTRY(_reset)

__stack_top = 0xFF002C00;

MEMORY
{
	FLASH (rx) : ORIGIN = 0xF8180000, LENGTH = 512K
}

SECTIONS
{
	.text : {
		KEEP(*(.header))
		*(.text .text.*)
		*(.rodata .rodata.* .sdata2 .sdata2.* .data.rel.ro .data.rel.ro.*)
		. = ALIGN(4);
	} > FLASH

	.data : { *(.data .data.* .sdata .sdata.*) } > FLASH
	.bss  : { *(.bss .bss.* .sbss .sbss.* COMMON) } > FLASH

	/DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}

ASSERT(SIZEOF(.data) == 0, "boot flash code must not have initialised data")
ASSERT(SIZEOF(.bss) == 0, "boot flash code must not have zero-initialised data")
ASSERT(_reset == 0xF8180100, "reset vector must be at offset 0x100")
