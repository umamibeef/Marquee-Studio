/* Umami app entry. The boot loader (bootloader/, our own source) decompresses this app to SDRAM address 0 and jumps
 * here after setting up the MMU. Contract (bootloader/src/main.c handoff_info + mmu_final):
 *   - word 0 is the entry: it branches over the header to the real start.
 *   - 0x14..0x98 is the boot-info block the boot loader fills (mapped RAM, clocks, NVRAM/flash geometry, SDRAM banks).
 *   - 0x4C (MMU table pointer) and 0x50 (table length) are read by the boot loader and NOT written by it: we leave
 *     them 0 so it takes the no-page-table path (mmu_final(0,0): the MMU is set up by direct TLB loads).
 * Built entirely from our source; no byte comes from the Mikohn dump. */
	.section .text.start, "ax"
	.globl _start
_start:
	b     _entry              /* word 0: jump over the boot-info header */
	.org  0xA0, 0             /* 0x04..0xA0 reserved and zero -> 0x4C/0x50 = 0 */
	/* exception vectors: report and stop, and the watchdog resets the board */
.org 0x100
	mtsprg 0, %r3
	li    %r3, 0x100
	b     _fault
.org 0x200
	mtsprg 0, %r3
	li    %r3, 0x200
	b     _fault
.org 0x300
	mtsprg 0, %r3
	li    %r3, 0x300
	b     _fault
.org 0x400
	mtsprg 0, %r3
	li    %r3, 0x400
	b     _fault
.org 0x500
	mtsprg 0, %r3
	mfmsr %r3
	ori   %r3, %r3, 0x30      /* MMU back on, as the stock entry does: the CPU cleared IR/DR */
	mtmsr %r3
	isync
	b     _extint
.org 0x600
	mtsprg 0, %r3
	li    %r3, 0x600
	b     _fault
.org 0x700
	mtsprg 0, %r3
	li    %r3, 0x700
	b     _fault
.org 0x800
	mtsprg 0, %r3
	li    %r3, 0x800
	b     _fault
.org 0x900
	/* the decrementer: nothing uses it, but with MSR[EE] on for the panel it fires each time it goes negative (it runs
	 * from reset, and the sim doesn't model it). Put it as far away as it goes and carry on. */
	mtsprg 0, %r3
	lis   %r3, 0x7FFF
	ori   %r3, %r3, 0xFFFF
	mtspr 22, %r3             /* DEC */
	mfsprg %r3, 0
	rfi
.org 0xA00
	mtsprg 0, %r3
	li    %r3, 0xA00
	b     _fault
.org 0xB00
	mtsprg 0, %r3
	li    %r3, 0xB00
	b     _fault
.org 0xC00
	mtsprg 0, %r3
	li    %r3, 0xC00
	b     _fault
.org 0xD00
	mtsprg 0, %r3
	li    %r3, 0xD00
	b     _fault
.org 0xE00
	mtsprg 0, %r3
	li    %r3, 0xE00
	b     _fault
.org 0xF00
	mtsprg 0, %r3
	li    %r3, 0xF00
	b     _fault
.org 0x1000
	mtsprg 0, %r3
	li    %r3, 0x1000
	b     _fault
.org 0x1100
	mtsprg 0, %r3
	li    %r3, 0x1100
	b     _fault
.org 0x1200
	mtsprg 0, %r3
	li    %r3, 0x1200
	b     _fault
.org 0x1300
	mtsprg 0, %r3
	li    %r3, 0x1300
	b     _fault
.org 0x1400
	mtsprg 0, %r3
	li    %r3, 0x1400
	b     _fault
.org 0x1500
	mtsprg 0, %r3
	li    %r3, 0x1500
	b     _fault
.org 0x1600
	mtsprg 0, %r3
	li    %r3, 0x1600
	b     _fault
.org 0x1700
	mtsprg 0, %r3
	li    %r3, 0x1700
	b     _fault
.org 0x1800
	mtsprg 0, %r3
	li    %r3, 0x1800
	b     _fault
.org 0x1900
	mtsprg 0, %r3
	li    %r3, 0x1900
	b     _fault
.org 0x1A00
	mtsprg 0, %r3
	li    %r3, 0x1A00
	b     _fault
.org 0x1B00
	mtsprg 0, %r3
	li    %r3, 0x1B00
	b     _fault
.org 0x1C00
	mtsprg 0, %r3
	li    %r3, 0x1C00
	b     _fault
.org 0x1D00
	mtsprg 0, %r3
	li    %r3, 0x1D00
	b     _fault
.org 0x1E00
	mtsprg 0, %r3
	li    %r3, 0x1E00
	b     _fault
.org 0x1F00
	mtsprg 0, %r3
	li    %r3, 0x1F00
	b     _fault
.org 0x2000
	/* real mode: keep every register in crash_regs (crash.c), then report on a stack of its own */
_fault:
	mtsprg 1, %r4
	lis   %r4, crash_regs@ha
	addi  %r4, %r4, crash_regs@l
	stw   %r0, 0(%r4)
	stw   %r1, 4(%r4)
	stw   %r2, 8(%r4)
	mfsprg %r0, 0
	stw   %r0, 12(%r4)
	mfsprg %r0, 1
	stw   %r0, 16(%r4)
	stmw  %r5, 20(%r4)
	mflr  %r0
	stw   %r0, 128(%r4)
	mfcr  %r0
	stw   %r0, 132(%r4)
	mfctr %r0
	stw   %r0, 136(%r4)
	mfxer %r0
	stw   %r0, 140(%r4)
	mfsrr0 %r0
	stw   %r0, 144(%r4)
	mfsrr1 %r0
	stw   %r0, 148(%r4)
	lis   %r1, __stack_top@ha
	addi  %r1, %r1, __stack_top@l
	bl    crash_report
4:	b     4b

	/* reload crash_regs and return to the interrupted instruction (GDB `c`/`s`) */
	.globl gdb_rfi
gdb_rfi:
	lis   %r3, crash_regs@ha
	addi  %r3, %r3, crash_regs@l
	lwz   %r0, 144(%r3)
	mtsrr0 %r0
	lwz   %r0, 148(%r3)
	mtsrr1 %r0
	lwz   %r0, 128(%r3)
	mtlr  %r0
	lwz   %r0, 132(%r3)
	mtcr  %r0
	lwz   %r0, 136(%r3)
	mtctr %r0
	lwz   %r0, 140(%r3)
	mtxer %r0
	lmw   %r5, 20(%r3)
	lwz   %r0, 0(%r3)
	lwz   %r1, 4(%r3)
	lwz   %r2, 8(%r3)
	lwz   %r4, 16(%r3)
	lwz   %r3, 12(%r3)
	rfi

	/* external interrupt (0x500): the IDMA panel ISR. The vector did mtsprg 0,r3. Save the volatile state on
	 * the interrupted stack (r1 is valid; interrupts are off), call the C handler, restore, and rfi. */
_extint:
	mtsprg 1, %r4
	stwu  %r1, -160(%r1)
	stw   %r0, 12(%r1)
	mfsprg %r0, 0
	stw   %r0, 16(%r1)          /* original r3 */
	mfsprg %r0, 1
	stw   %r0, 20(%r1)          /* original r4 */
	stw   %r5, 24(%r1)
	stw   %r6, 28(%r1)
	stw   %r7, 32(%r1)
	stw   %r8, 36(%r1)
	stw   %r9, 40(%r1)
	stw   %r10, 44(%r1)
	stw   %r11, 48(%r1)
	stw   %r12, 52(%r1)
	mflr  %r0
	stw   %r0, 56(%r1)
	mfcr  %r0
	stw   %r0, 60(%r1)
	mfctr %r0
	stw   %r0, 64(%r1)
	mfxer %r0
	stw   %r0, 68(%r1)
	mfsrr0 %r0
	stw   %r0, 72(%r1)
	mfsrr1 %r0
	stw   %r0, 76(%r1)
	bl    ext_interrupt
	lwz   %r0, 76(%r1)
	mtsrr1 %r0
	lwz   %r0, 72(%r1)
	mtsrr0 %r0
	lwz   %r0, 68(%r1)
	mtxer %r0
	lwz   %r0, 64(%r1)
	mtctr %r0
	lwz   %r0, 60(%r1)
	mtcr  %r0
	lwz   %r0, 56(%r1)
	mtlr  %r0
	lwz   %r12, 52(%r1)
	lwz   %r11, 48(%r1)
	lwz   %r10, 44(%r1)
	lwz   %r9, 40(%r1)
	lwz   %r8, 36(%r1)
	lwz   %r7, 32(%r1)
	lwz   %r6, 28(%r1)
	lwz   %r5, 24(%r1)
	lwz   %r4, 20(%r1)
	lwz   %r3, 16(%r1)
	lwz   %r0, 12(%r1)
	addi  %r1, %r1, 160
	rfi
_entry:
	/* stack */
	lis   %r1, __stack_top@ha
	addi  %r1, %r1, __stack_top@l
	li    %r0, 0
	stwu  %r0, -16(%r1)       /* a null back-chain frame */
	/* clear .bss */
	lis   %r3, __bss_start@ha
	addi  %r3, %r3, __bss_start@l
	lis   %r4, __bss_end@ha
	addi  %r4, %r4, __bss_end@l
1:	cmplw %r3, %r4
	bge   2f
	stw   %r0, 0(%r3)
	addi  %r3, %r3, 4
	b     1b
2:	/* take machine checks as exceptions (0x200 -> crash_report) instead of silent checkstops */
	mfmsr %r3
	ori   %r3, %r3, 0x1000    /* MSR[ME] */
	mtmsr %r3
	isync
	bl    app_main
3:	b     3b                  /* app_main should not return */
