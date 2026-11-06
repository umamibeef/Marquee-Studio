/* Patch code lives in SDRAM the bootloader takes from the bottom of the C heap
 * (it raises the heap-start offset at RAM 0x10). Must not use writable data. */
OUTPUT_ARCH(powerpc)
ENTRY(emoji_hook)
PATCH_BASE = 0x00C60000;
font_draw_char_cont = 0x00042DB4;
SECTIONS
{
	. = PATCH_BASE;
	.text : { KEEP(*(.text)) *(.text.*) *(.rodata .rodata.*) . = ALIGN(4); }
	.data : { *(.data .data.* .sdata .sdata.* .bss .bss.* .sbss COMMON) }
	/DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
ASSERT(SIZEOF(.data) == 0, "patch code must not have writable data")
ASSERT(emoji_hook == PATCH_BASE, "the hook must be first")
