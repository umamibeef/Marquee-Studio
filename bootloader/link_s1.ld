/* Stage 1 of the two-stage boot loader (include/serdump.h): the DUMP boot loader built with STAGE1=1, run from SDRAM.
 * Stage 0 copies header + code to S1_LOAD (0x00F00000); the code is linked right after the 32-byte header, entry
 * first. No writable data, as in the boot flash build. */
OUTPUT_ARCH(powerpc)
ENTRY(_s1_entry)

__stack_top = 0xFF002C00;

MEMORY
{
	RAM (rwx) : ORIGIN = 0x00F00020, LENGTH = 64K - 32
}

SECTIONS
{
	.text : {
		KEEP(*(.s1entry))
		*(.text .text.*)
		*(.rodata .rodata.* .sdata2 .sdata2.* .data.rel.ro .data.rel.ro.*)
		. = ALIGN(4);
	} > RAM

	.data : { *(.data .data.* .sdata .sdata.*) } > RAM
	.bss  : { *(.bss .bss.* .sbss .sbss.* COMMON) } > RAM

	/DISCARD/ : { *(.header) *(.comment) *(.eh_frame*) *(.note*) }
}

ASSERT(SIZEOF(.data) == 0, "stage 1 must not have initialised data")
ASSERT(SIZEOF(.bss) == 0, "stage 1 must not have zero-initialised data")
ASSERT(_s1_entry == 0x00F00020, "stage 1 entry must be right after the header")
