/* Chip selects for NVRAM, main flash and CS7 on the SPD-configured board variant.
 * Translated from 0xF8181240.
 * Sizes come from board straps: PIPR (PCMCIA input pins) for flash bank 1,
 * PBDAT bits for flash bank 2, and PDDAT bits for wait states and the CS7 device. */
#include "board.h"
#include "tables.h"

void memmap_from_straps(uintptr_t cfg)
{
	uint32_t opts = cfg32(cfg, CFG_OPTIONS);
	uint32_t nv, g, sz, t1 = 0, t2 = 0, cs7 = 0;
	const uint8_t *wait;
	uint8_t strap;

	/* NVRAM on CS1 */
	BI->nvram_base = nvram_br[1] & 0xFFFF8000;
	nv = nvram_sizes[(opts & OPT_NVRAM_MASK) >> 29];
	BI->nvram_size = nv;
	if (nv) {
		OR(1) = 0x940 | ~(nv - 1);
		BR(1) = nvram_br[1];
	}

	BI->cs7_base = CS7_BASE;
	BI->flash1_base = FLASH1_BASE;
	BI->flash2_base = FLASH2_BASE;

	wait = cfg32(cfg, CFG_SYSCLK) < 48000000 ? flash_wait_slow : flash_wait_fast;

	/* Flash bank 1: geometry from PIPR bits 3..6, wait states from PIPR bits 0..2 */
	strap = PIPR_HI;
	g = flash_geom[(strap >> 3) & 0xF];
	sz = g & ~7u;
	if (sz) {
		uint32_t ws = wait[strap & 7];

		t1 = sz * (g & 7);
		BI->flash1_total = t1;

		/* Flash bank 2: geometry from PBDAT bits 14..17, wait states from PDDAT bits 9..11 */
		g = flash_geom[(PBDAT >> 14) & 0xF];
		sz = g & ~7u;
		if (sz) {
			uint32_t ws2 = wait[(PDDAT >> 9) & 7];

			t2 = sz * (g & 7);
			BI->flash2_total = t2;
			if (ws2 > ws)
				ws = ws2;
		}

		/* CS6 spans both banks in one 256 MB window, 32-bit, GPCM */
		OR(6) = 0xF0000000 | (ws << 4) | 0x900;
		BR(6) = FLASH1_BASE | 1;
	}

	/* CS7 (2 MB, 8-bit) when there is no second flash bank, if forced by the config
	 * or if the PD8 strap reads low. */
	if (t2 == 0 && ((opts & OPT_CS7_ALWAYS) || !(PDDAT & 0x80))) {
		OR(7) = 0xFFE00940;              /* mask first, then base + valid */
		BR(7) = CS7_BASE | 0x401;
		cs7 = 0x200000;
	}

	BI->flash1_size = t1;
	BI->flash2_size = t2;
	BI->cs7_size = cs7;
}
