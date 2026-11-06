/* The Umami app runs from SDRAM address 0 (bootloader/include/board.h APP_LOAD). An ordinary relocatable link:
 * the linker lays the image out from 0; adding or removing a function changes it like any normal program.
 * RAM after the image: the stack, then the heap up to the top of RAM (rt.c). */
ENTRY(_start)
SECTIONS
{
	. = 0x00000000;
	.text : { KEEP(*(.text.start)) *(.text .text.*) *(.rodata .rodata.* .sdata2 .sdata2.*) }
	.data : { *(.data .data.* .sdata .sdata.*) }
	. = ALIGN(4);
	__bss_start = .;
	.bss  : { *(.bss .bss.* .sbss .sbss.* COMMON) }
	. = ALIGN(4);
	__bss_end = .;
	. = ALIGN(16);
	. = . + 0x40000;
	__stack_top = .;
	__heap_start = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
