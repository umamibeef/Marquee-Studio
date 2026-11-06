/* The debug record (include/dbg.h) on the boot loader's side, DUMP builds only.
 *
 * Every boot gets an entry with its reset cause (RSR, cleared afterwards so each boot shows its own) and the last
 * step it reached. And the boot loader catches its own exceptions: the MPC862 starts with MSR[IP] = 1 (vectors at
 * 0xFFF00000), which only work while CS0 still answers everywhere; once BR0 moves the boot flash to 0xF8180000 an
 * exception fetches from nothing and the CPU checkstops without a trace. So once the SDRAM works, stubs at
 * 0x100..0x1400 send every vector to dbg_exc_common (start.S) and MSR[IP] is cleared. dbg_exception records the
 * crash in NVRAM and lets the watchdog reset the board. Loading the app overwrites address 0, so dbg_vectors_off
 * puts IP back before that. */
#ifdef DUMP
#include "board.h"
#include "dbg.h"

#define D ((volatile struct dbg_area *)(NVRAM_BASE + DBG_OFFSET))

void dbg_exc_common(void);

/* After the serial window the data cache is on in real mode with copy-back as the default (MD_CTR = 0), so a store
 * to NVRAM would stay in the cache until mmu_final flushes it, and be lost if the board resets first. Write the
 * lines back straight away. */
void dbg_flush(volatile const void *p, uint32_t n)
{
	uint32_t a = (uint32_t)(uintptr_t)p & ~15u, e = (uint32_t)(uintptr_t)p + n;

	for (; a < e; a += 16)
		__asm__ volatile("dcbst 0, %0" :: "r"(a) : "memory");
	__asm__ volatile("sync" ::: "memory");
}

void dbg_boot_start(void)
{
	uint32_t rsr = RSR, i;
	volatile struct dbg_boot *b;

	RSR = rsr;                                       /* write 1 to clear: the next boot sees only its own cause */
	if (D->magic != DBG_MAGIC) {
		for (i = 0; i < DBG_SIZE; i++)
			((volatile uint8_t *)D)[i] = 0;
		D->magic = DBG_MAGIC;
	}
	D->seq = D->seq + 1;
	b = dbg_cur(D);
	b->seq = D->seq;
	b->rsr = rsr;
	b->step = STEP_BOOT;
	b->pad = 0;
	b->value = 0;
	dbg_flush(D, 16);
	dbg_flush(b, sizeof *b);
}

void dbg_step(uint16_t step, uint32_t value)
{
	dbg_mark(D, step, value);
	dbg_flush(dbg_cur(D), sizeof(struct dbg_boot));
}

uint8_t dbg_options(void)
{
	return D->magic == DBG_MAGIC ? D->options : 0;
}

void dbg_set_options(uint8_t v)
{
	D->options = v;
	dbg_flush(D, 16);
}

uint8_t dbg_s1_tries(void)
{
	return D->magic == DBG_MAGIC ? D->s1_tries : 0;
}

void dbg_set_s1_tries(uint8_t v)
{
	D->s1_tries = v;
	dbg_flush(D, 16);
}

void dbg_clear_crash(void)
{
	D->crash.magic = 0;
	dbg_flush(&D->crash, 4);
}

static uint32_t mtspr_r(uint32_t spr, uint32_t rs)  /* mtspr spr, rS */
{
	return 0x7C0003A6u | rs << 21 | (spr & 0x1F) << 16 | (spr >> 5) << 11;
}

/* SDRAM works: a stub at every vector, then vectors from address 0 (MSR[IP] = 0). */
void dbg_vectors_on(void)
{
	uint32_t v, a = (uint32_t)(uintptr_t)dbg_exc_common;

	for (v = 1; v <= 0x14; v++) {
		volatile uint32_t *p = (volatile uint32_t *)(v << 8);

		p[0] = mtspr_r(272, 1);                  /* mtsprg 0, r1 */
		p[1] = mtspr_r(273, 0);                  /* mtsprg 1, r0 */
		p[2] = 0x3C000000u | a >> 16;            /* lis r0, common@h */
		p[3] = 0x60000000u | (a & 0xFFFF);       /* ori r0, r0, common@l */
		p[4] = 0x7C0903A6u;                      /* mtctr r0 */
		p[5] = 0x38000000u | v;                  /* li r0, vector >> 8 */
		p[6] = mtspr_r(274, 0);                  /* mtsprg 2, r0 */
		p[7] = 0x4E800420u;                      /* bctr */
	}
	__asm__ volatile("sync; isync" ::: "memory");
	mtmsr(mfmsr() & ~0x40u);
	__asm__ volatile("isync");
}

void dbg_vectors_off(void)
{
	uint32_t msr = mfmsr();

	if (msr & 0x40u)                                 /* already back (called from more than one load path) */
		return;
	mtmsr(msr | 0x40u);
	__asm__ volatile("isync");
}

/* From dbg_exc_common: the GPRs are already in the record. Never returns. */
void dbg_exception(uint32_t vec, uint32_t lr, uint32_t cr)
{
	volatile struct dbg_crash *c = &D->crash;
	volatile struct dbg_boot *b = dbg_cur(D);

	c->seq = D->seq;
	c->source = DBG_SRC_BOOTLOADER;
	c->vector = vec << 8;
	c->srr0 = MFSPR(26);
	c->srr1 = MFSPR(27);
	c->dar = MFSPR(19);
	c->dsisr = MFSPR(18);
	c->lr = lr;
	c->cr = cr;
	c->step = b->step;
	c->value = b->value;
	c->magic = DBG_CRASH_MAGIC;
	dbg_flush(c, sizeof *c);
	for (;;)                                         /* no watchdog service: the board resets in ~2.7 s */
		;
}
#endif
