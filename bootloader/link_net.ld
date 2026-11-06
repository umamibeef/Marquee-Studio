/* NET=1 build: as link.ld, plus a writable data region for lwIP in SDRAM.
 * .data is stored in flash and copied to SDRAM by net_crt_init(); .bss is zeroed there.
 * Only the network window may touch them, because SDRAM does not work before the memory
 * controller is set up. The Makefile checks that no other object has .data/.bss. */
OUTPUT_ARCH(powerpc)
ENTRY(_reset)

__stack_top = 0xFF002C00;

MEMORY
{
	FLASH  (rx)  : ORIGIN = 0xF8180000, LENGTH = 512K
	NETRAM (rwx) : ORIGIN = 0x00680000, LENGTH = 512K   /* NET_DATA_BASE in netboot.h */
}

SECTIONS
{
	.text : {
		KEEP(*(.header))
		*(.text .text.*)
		*(.rodata .rodata.* .sdata2 .sdata2.* .data.rel.ro .data.rel.ro.*)
		. = ALIGN(4);
	} > FLASH

	.data : {
		__net_data_start = .;
		*(.data .data.* .sdata .sdata.*)
		. = ALIGN(4);
		__net_data_end = .;
	} > NETRAM AT > FLASH
	__net_data_load = LOADADDR(.data);

	.bss (NOLOAD) : {
		__net_bss_start = .;
		*(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(4);
		__net_bss_end = .;
	} > NETRAM

	/DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}

ASSERT(_reset == 0xF8180100, "reset vector must be at offset 0x100")
ASSERT(__net_bss_end <= 0x00700000, "lwIP data overlaps the FEC area")
