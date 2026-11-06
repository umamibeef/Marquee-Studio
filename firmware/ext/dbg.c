/* The debug record in NVRAM (bootloader/include/dbg.h) on the firmware's side: the steps the firmware reaches and its
 * crash reports. Only written when our boot loader has started the record (its magic). */
#include <stdint.h>
#include "../../bootloader/include/dbg.h"

#define NVRAM_BASE (*(volatile uint32_t *)0x24)
#define DBGA ((volatile struct dbg_area *)(NVRAM_BASE + DBG_OFFSET))

void fw_step(uint16_t step, uint32_t value)
{
	dbg_mark(DBGA, step, value);
}

/* firmware/ext/crash.c: the crash it reports on the DB9, also into the record. vector is the RTEMS index. */
void fw_crash_record(uint32_t vector, uint32_t srr0, uint32_t srr1, uint32_t dar, uint32_t dsisr, uint32_t lr,
                     uint32_t cr, const uint32_t *r)
{
	static const uint16_t OFF[16] = {0, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800,
	                                 0x900, 0xA00, 0xB00, 0xC00, 0xD00, 0xE00, 0x1000, 0};
	volatile struct dbg_area *d = DBGA;
	volatile struct dbg_crash *c = &d->crash;
	volatile struct dbg_boot *b;
	unsigned i;

	if (d->magic != DBG_MAGIC)
		return;
	b = dbg_cur(d);
	c->seq = d->seq;
	c->source = DBG_SRC_FIRMWARE;
	c->vector = vector < 16 ? OFF[vector] : vector;
	c->srr0 = srr0;
	c->srr1 = srr1;
	c->dar = dar;
	c->dsisr = dsisr;
	c->lr = lr;
	c->cr = cr;
	c->step = b->step;
	c->value = b->value;
	for (i = 0; i < 32; i++)
		c->gpr[i] = r[i];
	c->magic = DBG_CRASH_MAGIC;
}

