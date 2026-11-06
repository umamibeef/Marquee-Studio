/* BOOTSTRAP V2.00 entry, archive header and hand-off.
 *
 * The flash starts with archive entry 0 (the bootstrap itself). The MPC862 resets to
 * offset 0x100 of the boot device. Until BR0 is written, CS0 answers at every address,
 * so the absolute jump to the link address works straight away. */

#ifdef STAGE1
/* Stage 1 (serdump.h, two stages): stage 0 calls it with r3 = the config buffer. A fresh stack in DPRAM (stage 0's
 * frames are not needed again), then stage1_main, which never returns. */
	.section .s1entry, "ax"
	.globl	_s1_entry
_s1_entry:
	lis	%r1, __stack_top@ha
	addi	%r1, %r1, __stack_top@l
	li	%r0, 0
	stwu	%r0, -16(%r1)
	bl	stage1_main
1:	b	1b
#endif

	.section .header, "ax"
	.globl	_header
_header:
	.long	0			/* entry length incl. check word: set by tools/mkimage.py */
	.long	0			/* length - 4: set by tools/mkimage.py */
	.long	0			/* flags: not bootable, stored */
	.byte	15
	.ascii	"BOOTSTRAP V2.00"

	.org	0x100
	.globl	_reset
_reset:					/* 0xF8180100 */
	nop
	li	%r3, 0x1042		/* ME | IP | RI */
	mtmsr	%r3
	mtsrr1	%r3
	lis	%r4, 0xFF00		/* IMMR = 0xFF000000 */
	mtspr	638, %r4
	lis	%r3, _entry@ha
	addi	%r3, %r3, _entry@l
	mtlr	%r3
	blr

_entry:
	/* C needs a stack; the original's 64 bytes below 0xFF002040 are not enough. */
	lis	%r1, __stack_top@ha
	addi	%r1, %r1, __stack_top@l
	li	%r0, 0
	stwu	%r0, -16(%r1)
	bl	boot_main
1:	b	1b

/* int call_on_stack(int (*fn)(void *), void *arg, uint32_t stack_top)
 * Run fn(arg) with r1 = stack_top - 16, then return to the caller's stack. Frame layout:
 * 0 = back chain (caller's r1), 4 = LR slot for fn, 8 = our saved LR. */
	.text
	.globl	call_on_stack
call_on_stack:
	mflr	%r0
	addi	%r5, %r5, -16
	stw	%r1, 0(%r5)
	stw	%r0, 8(%r5)
	mr	%r1, %r5
	mtctr	%r3
	mr	%r3, %r4
	bctrl
	lwz	%r0, 8(%r1)
	lwz	%r1, 0(%r1)
	mtlr	%r0
	blr

/* void boot_handoff(int status)  -- 0xF818083C..0xF81808AC
 * Turn on address translation with the tables mmu_final*() loaded, enable the caches and
 * jump to the application at address 0. A nonzero status (no image) hangs instead. */
	.text
	.globl	boot_handoff
boot_handoff:
	mr	%r31, %r3
	li	%r3, 0x1032		/* ME | IR | DR | RI */
	mtmsr	%r3
	mtsrr1	%r3
	isync
	sync
	lis	%r3, 0x0A00		/* invalidate */
	mtspr	560, %r3
	isync
	sync
	mtspr	568, %r3
	lis	%r3, 0x0C00		/* unlock all */
	mtspr	560, %r3
	isync
	sync
	mtspr	568, %r3
	lis	%r3, 0x0200		/* enable */
	mtspr	560, %r3
	isync
	sync
	mtspr	568, %r3
	li	%r3, 0
	mtspr	149, %r3		/* DER = 0 */
	cmplwi	%r31, 0
	bne	2f
	lis	%r2, 0
	mtlr	%r2
	blr				/* -> application entry at 0 */
2:	b	2b

#ifdef DUMP
/* Every vector's stub (dbg.c) arrives here with r1 in SPRG0, r0 in SPRG1, the vector >> 8 in SPRG2 and CTR used.
 * Save the GPRs into the NVRAM crash record, then dbg_exception(vector, LR, CR) on a fresh stack (never returns). */
	.text
	.globl	dbg_exc_common
dbg_exc_common:
	lis	%r1, (0xF8780000 + 0x7C00 + 0x100)@ha
	addi	%r1, %r1, (0xF8780000 + 0x7C00 + 0x100)@l
	stmw	%r2, (0x30 + 8)(%r1)
	mfsprg	%r0, 1
	stw	%r0, 0x30(%r1)
	mfsprg	%r0, 0
	stw	%r0, 0x34(%r1)
	mflr	%r4
	mfcr	%r5
	mfsprg	%r3, 2
	lis	%r1, __stack_top@ha
	addi	%r1, %r1, __stack_top@l
	li	%r0, 0
	stwu	%r0, -64(%r1)
	bl	dbg_exception
1:	b	1b
#endif
