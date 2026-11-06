/* MPC8xx MMU and cache set-up. Translated from:
 *   0xF8182320 mmu_init_spd     - temporary map so SDRAM and flash work during the load (SPD path)
 *   0xF8181480 mmu_final        - final map handed to the app (SPD path)
 *   0xF8181870 mmu_final_fixed  - final map handed to the app (fixed-config path)
 * plus their helpers: TLB loader (0xF81817EC = 0xF8181B8C = 0xF81825FC, identical copies) and
 * region mappers (0xF81816C0 = 0xF818250C, and the 8 MB-only 0xF8181AA0).
 *
 * The app passes a descriptor table (pointer and length at RAM 0x4C/0x50 of the loaded image);
 * the mappers record each mapping there as well as loading it into the TLBs. */
#include "board.h"

/* Mapping flags (r8 in the original) */
#define MAP_CI   0x01   /* cache-inhibited */
#define MAP_G    0x02   /* guarded */
#define MAP_8M   0x04   /* 8 MB pages, else 512 KB */
#define MAP_I    0x10   /* load into the instruction TLB */
#define MAP_D    0x20   /* load into the data TLB */

/* TLB fill state in DPRAM: next free index and lowest index, per TLB */
struct tlbcnt {
	uint32_t i_next, d_next, i_min, d_min, saved;
};

static void tlb_load(volatile struct tlbcnt *c, uint32_t epn, uint32_t twc, uint32_t rpn,
                     uint32_t flags)
{
	if ((flags & MAP_I) && c->i_next > c->i_min) {
		MTSPR(SPR_MI_EPN, epn);
		MTSPR(SPR_MI_TWC, twc);
		MTSPR(SPR_MI_RPN, rpn);
		c->i_next--;
	}
	if ((flags & MAP_D) && c->d_next > c->d_min) {
		MTSPR(SPR_MD_EPN, epn);
		MTSPR(SPR_MD_TWC, twc);
		MTSPR(SPR_MD_RPN, rpn);
		c->d_next--;
	}
}

/* Map [phys, phys+len) at virt in pages of `page` bytes. Runs at least once, as the
 * original (a do/while), so len <= 0 still maps one page.
 * only8m selects the 0xF8181AA0 variant: always 8 MB pages, simpler table bookkeeping. */
static void map_region(volatile struct tlbcnt *c, uint32_t *table, uint32_t page, int32_t len,
                       uint32_t flags, uint32_t phys, uint32_t virt, int only8m)
{
	uint32_t twc, mask;

	if (only8m || (flags & MAP_8M)) {
		twc = 0xD;              /* V, 8 MB */
		mask = 0xFF800000;
	} else {
		twc = 0x5;              /* V, 512 KB */
		mask = 0xFFF80000;
	}
	if (flags & MAP_G)
		twc |= 2;

	do {
		uint32_t rpn = (phys & mask) | 0x9FD;

		if (flags & MAP_CI)
			rpn |= 2;
		if (table) {
			if (!only8m && (phys >> 28) == 5) {
				table[0] = twc;
				table[1] = rpn & 0xFFF;
			} else {
				uint32_t idx;

				table[2] = twc;
				if (only8m || ((twc >> 2) & 3) == 3)
					idx = (virt >> 21) & 0x7C;
				else
					idx = (virt >> 17) & 0x1FC;
				if (idx < 512)
					*(uint32_t *)((uintptr_t)table + 12 + idx) = rpn;
			}
		}
		tlb_load(c, (virt & 0xFFFFF000) | 0x200, twc, rpn, flags);
		phys += page;
		virt += page;
		len -= (int32_t)page;
	} while (len > 0);
}

static void tlb_reset(volatile struct tlbcnt *c, uint32_t *table, uint32_t table_len)
{
	uint32_t *p, *end = (uint32_t *)((uintptr_t)table + table_len);

	tlbia();
	MTSPR(SPR_M_CASID, 0);
	MTSPR(SPR_MI_CTR, 0x20001F00);
	MTSPR(SPR_MD_CTR, 0x34001F00);
	c->i_next = 32;
	c->d_next = 32;
	c->i_min = 0;
	c->d_min = 0;
	for (p = table; p < end; p++)
		*p = 0;
	MTSPR(SPR_M_TWB, (uint32_t)(uintptr_t)table);

	tlb_load(c, 0xFF000200, 0x13, 0xFF0009FF, MAP_D);           /* IMMR + DPRAM */
	tlb_load(c, 0xF8000200, 0x0F, 0xF80009FD, MAP_I | MAP_D);   /* boot flash, 8 MB */
}

static void caches_on(void)
{
	MTSPR(SPR_IC_CST, 0x0A000000); isync_sync(); MTSPR(SPR_DC_CST, 0x0A000000);  /* invalidate */
	MTSPR(SPR_IC_CST, 0x0C000000); isync_sync(); MTSPR(SPR_DC_CST, 0x0C000000);  /* unlock all */
	MTSPR(SPR_IC_CST, 0x02000000); isync_sync(); MTSPR(SPR_DC_CST, 0x02000000);  /* enable */
}

/* Flush the data cache by set/way, both ways, then disable both caches. */
static void caches_flush_off(uint32_t sets)
{
	uint32_t i;

	for (i = sets; i--; ) {
		MTSPR(SPR_DC_ADR, i << 4);
		MTSPR(SPR_DC_CST, 0x0E000000);
		(void)MFSPR(SPR_DC_CST);
	}
	for (i = sets; i--; ) {
		MTSPR(SPR_DC_ADR, (i << 4) | 0x1000);
		MTSPR(SPR_DC_CST, 0x0E000000);
		(void)MFSPR(SPR_DC_CST);
	}
	MTSPR(SPR_IC_CST, 0x04000000);
	isync_sync();
	MTSPR(SPR_DC_CST, 0x04000000);
}

int mmu_init_spd(void)
{
	volatile struct tlbcnt *c = (volatile struct tlbcnt *)TLBCNT_C;

	mtmsr(mfmsr() & ~0x30u);                 /* IR/DR off */
	isync_sync();
	tlb_reset(c, 0, 0);
	tlb_load(c, 0x60000200, 0x0F, 0x600009FD, MAP_I | MAP_D);   /* CS7 */
	tlb_load(c, 0x00000200, 0x0D, 0x000009FD, MAP_I | MAP_D);   /* first 8 MB of SDRAM */

	/* main flash, data side only, guarded. The second call inherits the leftover length
	 * (0) from the first in the original, so it maps exactly one 8 MB page. */
	map_region(c, 0, 0x800000, 0x2000000, MAP_D | MAP_8M | MAP_G, FLASH1_BASE, FLASH1_BASE, 0);
	map_region(c, 0, 0x800000, 0, MAP_D | MAP_8M | MAP_G, FLASH2_BASE, FLASH2_BASE, 0);

	MTSPR(SPR_MI_CTR, 0x28001B00);
	MTSPR(SPR_MD_CTR, 0x3C001B00);
	caches_on();
	mtmsr(mfmsr() | 0x30);                   /* IR/DR on */
	isync_sync();
	return 0;
}

static void map_sdram(volatile struct tlbcnt *c, uint32_t *table, uint32_t page, uint32_t flags,
                      int only8m)
{
	const uint32_t *bank = BI->bank_list;
	uint32_t size = BI->bank_size, virt = 0;

	BI->boot_virt = BI->boot_base;
	BI->cs7_virt = BI->cs7_base;
	BI->nvram_virt = BI->nvram_base;

	c->d_min = 0;
	c->saved = c->d_next;
	BI->mapped_ram = 0;
	for (; *bank != 0xFFFFFFFF; bank++) {    /* banks appear back to back from virtual 0 */
		BI->mapped_ram += size;
		map_region(c, table, page, (int32_t)size, flags, *bank, virt, only8m);
		virt += size;
	}
	c->saved = c->d_next;
	c->d_min = 0;
	MTSPR(SPR_MI_CTR, 0x28001B00);           /* lock the loaded entries */
	MTSPR(SPR_MD_CTR, 0x3C001B00);
}

int mmu_final(uint32_t *table, uint32_t table_len)
{
	volatile struct tlbcnt *c = (volatile struct tlbcnt *)TLBCNT_A;
	uint32_t page = 0x800000, flags = MAP_I | MAP_D | MAP_8M;

	mtmsr(mfmsr() | 0x30);
	caches_flush_off(128);
	mtmsr(mfmsr() & ~0x30u);
	isync_sync();
	tlb_reset(c, table, table_len);

	if (BI->bank_size < 0x800000) {          /* small banks: 512 KB pages */
		page = 0x80000;
		flags = MAP_I | MAP_D;
	}
	map_sdram(c, table, page, flags, 0);
	return 0;
}

int mmu_final_fixed(uint32_t *table, uint32_t table_len)
{
	volatile struct tlbcnt *c = (volatile struct tlbcnt *)TLBCNT_B;

	caches_flush_off(256);
	mtmsr(mfmsr() & ~0x30u);
	isync_sync();
	tlb_reset(c, table, table_len);
	map_sdram(c, table, 0x800000, MAP_I | MAP_D | MAP_8M, 1);
	return 0;
}
