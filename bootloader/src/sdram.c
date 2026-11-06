/* SDRAM set-up from the module's SPD EEPROM (I2C 0xA2). Translated from 0xF81808C0.
 * Programs UPMA and UPMB with one of three timing tables, sizes the module from SPD
 * bytes 3, 4, 5, 12, 17 and 31, and builds the bank list used by the MMU set-up.
 * Returns 0, or 101 (bad row/column count), 102 (no density bit), 2 (refresh too slow).
 * The caller retries until it gets 0. */
#include "board.h"
#include "tables.h"

static void spin(uint32_t n)
{
	while (n--)
		__asm__ volatile("" ::: "memory");
}

int sdram_from_spd(uintptr_t cfg)
{
	volatile uint8_t *spd = (volatile uint8_t *)SPD_BUF;
	uint32_t *list = (uint32_t *)BANK_LIST;
	uint32_t sysclk = BI->sysclk;           /* read before the 40 MHz override below */
	const uint32_t *upm;
	uint32_t rows, cols, nmod, nbanks, dens, bit, mb, total_mb, bank_mb;
	uint32_t ref, ptp, div, amr, i, m, k;
	uint8_t sum, xr;

	do {
		while (i2c_read(0xA2, SPD_BUF, 256))
			;
		eeprom_sum(SPD_BUF, 63, &sum, &xr);
	} while (spd[63] != sum);                /* standard SPD checksum, byte 63 */

	BR(2) = 0;
	BR(3) = 0;
	BR(4) = 0;
	BR(5) = 0;
	MAMR = 0x9C124111;
	MBMR = 0x9C124111;
	spin(0xFFFF);

	if (cfg32(cfg, CFG_OPTIONS) & OPT_UPM_40MHZ) {
		BI->sysclk = 40000000;
		upm = upm_tables[0];
	} else {
		upm = sysclk > 40000000 ? upm_tables[2] : upm_tables[1];
	}

	for (i = 0; i < 64; i++) {               /* same program into UPMA and UPMB */
		MDR = upm[i];
		MCR = i;
		MCR = 0x00800000 + i;
	}
	/* SDRAM init sequences on CS2 and CS4 (precharge, refresh, mode register) */
	MCR = 0x80004105;
	MCR = 0x80004830;
	MAR = 0x88;
	MCR = 0x80004106;
	MCR = 0x80008105;
	MCR = 0x80008830;
	MAR = 0x88;
	MCR = 0x80008106;

	rows = spd[3];
	if (rows < 8 || rows > 14)
		return 101;
	cols = spd[4];
	if (cols < 8 || cols > 13)
		return 101;
	if (spd[5] == 0 || spd[5] > 2)
		spd[5] = 1;
	if (spd[17] == 0 || spd[17] > 4)
		spd[17] = 1;
	nbanks = spd[17];                         /* internal banks per device */
	nmod = spd[5];                            /* physical banks on the module */
	dens = spd[31];                           /* bank density bit map, bit 0 = 4 MB */

	bit = 0x80;
	for (k = 8; !(dens & bit); ) {
		bit >>= 1;
		if (--k == 0)
			return 102;
	}
	mb = bit << 2;
	total_mb = nmod * mb;
	bank_mb = (mb / 2) / nbanks;

	ref = spd[12] & 0x7F;
	if (ref > 5) {
		spd[12] = 0;
		ref = 0;
	}
	ptp = (sysclk / 1000) / refresh_div[ref];
	if (nmod == 2)
		ptp >>= 1;
	div = 8192;
	while (ptp >= 256) {
		ptp >>= 1;
		div >>= 1;
		if (div == 128)
			return 2;
	}
	MPTPR = (uint16_t)div;

	amr = amx_by_cols[cols - 8] | (ptp << 24);
	MBMR = amr;
	MAMR = amr | 0x00800000;

	OR(2) = 0xF0000E00;
	OR(3) = 0xF0000E00;
	OR(4) = 0xF0000E00;
	OR(5) = 0xF0000E00;

	for (i = 0; i < 17; i++)
		list[i] = 0;
	BR(2) = bank_bases[0] | 0x81;             /* UPMA */
	BR(3) = bank_bases[4] | 0xC1;             /* UPMB */
	if (nmod == 2) {
		BR(4) = bank_bases[8] | 0x81;
		BR(5) = bank_bases[12] | 0xC1;
	}

	k = 0;
	for (m = 0; m < nmod * 2; m++)
		for (i = 0; i < nbanks; i++)
			list[k++] = bank_bases[m * 4 + i];
	list[k] = 0xFFFFFFFF;

	BI->bank_list = list;
	BI->bank_size = bank_mb << 20;
	BI->sdram_total = total_mb << 20;
	return 0;
}
