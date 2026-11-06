/* BOOTSTRAP V2.00 main sequence. Translated from 0xF8180128..0xF81808AC.
 *
 *  1. Watchdog, debug, keys, clear the boot-info block, map this flash on CS0.
 *  2. Read the board config EEPROM (I2C 0xA0) until its sum/xor bytes check out.
 *  3. Memory set-up, one of two ways:
 *     - SPD path (cfg[4] == 1 and cfg[5] < 0x50): chip selects from straps, SDRAM from SPD,
 *       temporary MMU map, load the app, final MMU map.
 *     - fixed path: chip selects, PLL and UPM program all from the EEPROMs (0xA0, 0xA2),
 *       load the app, final MMU map.
 *  4. Hand over: boot info at RAM 0x14..0x98, then jump to address 0 (start.S). */
#include "board.h"
#include "archive.h"
#ifdef DUMP
#include "serdump.h"
#endif

static void spin(uint32_t n)
{
	while (n--)
		__asm__ volatile("" ::: "memory");
}

/* PLL: multiply the oscillator up to the wanted system clock. Returns the PLPRCR value. */
static uint32_t pll_setup(uintptr_t cfg)
{
	uint32_t osc = cfg32(cfg, CFG_OSCCLK);
	uint32_t mf = cfg32(cfg, CFG_SYSCLK) / osc;

	if (mf)
		mf--;
	mf <<= 20;
	BI->sysclk = ((mf >> 20) + 1) * osc;
	return mf;
}

static void read_cfg(uintptr_t cfg)
{
	uint8_t sum, xr;

	do {
		while (i2c_read(0xA0, cfg, 256))
			;
		eeprom_sum(cfg, 62, &sum, &xr);
	} while (cfg8(cfg, CFG_SUM) != sum || cfg8(cfg, CFG_XOR) != xr);
}

/* Fixed-config path: everything comes from the two EEPROMs. 0xF8180228..0xF8180610 */
int boot_fixed_rest(uintptr_t cfg, int window_done);

#if defined(DUMP) && !defined(STAGE1)
/* Two stages (serdump.h): with a stage 1 in the loader sector, a short recovery window, then copy stage 1 into SDRAM
 * and run it. Returns (without stage 1) 1 if the recovery window was served, else 0. The caches are off here. */
static int stage0_try(void)
{
	uint32_t a = loader_where(), i, n;
	const volatile struct s1_header *h = (const volatile struct s1_header *)a;

	if (!a || h->magic != S1_MAGIC)
		return 0;                                /* none installed: exactly as before */
	if (dump_window_recovery()) {
		DBG_STEP(STEP_S1_SKIPPED, 1);
		return 1;
	}
	if (!s1_valid(a)) {
		DBG_STEP(STEP_S1_SKIPPED, 2);
		return 0;
	}
	if (dbg_s1_tries() >= 2) {
		DBG_STEP(STEP_S1_SKIPPED, 3);
		return 0;
	}
	dbg_set_s1_tries((uint8_t)(dbg_s1_tries() + 1));
	n = S1_HDR + h->len;
	for (i = 0; i < n; i += 4) {
		*(volatile uint32_t *)(S1_LOAD + i) = *(const volatile uint32_t *)(a + i);
		if (!(i & 0x3FFF))
			wdt_kick();
	}
	__asm__ volatile("sync; isync" ::: "memory");
	if (!s1_valid(S1_LOAD)) {                        /* the copy in SDRAM, as it will run */
		DBG_STEP(STEP_S1_SKIPPED, 2);
		return 0;
	}
	dbg_vectors_on();                                /* a stage 1 that faults at once still leaves a crash record */
	DBG_STEP(STEP_STAGE1, a);
	((void (*)(uintptr_t))(S1_LOAD + ((const volatile struct s1_header *)S1_LOAD)->entry))(CFG_BUF);
	return 0;                                        /* not reached: stage 1 hands off to the app itself */
}
#endif

static int boot_fixed(uintptr_t cfg)
{
	volatile uint32_t *banks = (volatile uint32_t *)BANK_LIST;
	uint32_t v, size1, i;
	uint8_t code;

	OR(0) = cfg32(cfg, CFG_OR0);
	BI->nvram_base = NVRAM_BASE;
	BR(1) = NVRAM_BASE | 0x401;
	v = cfg32(cfg, CFG_OR1);
	OR(1) = v;
	BI->nvram_size = -(v & ~0x7FFFu);
#ifdef DUMP
	dbg_boot_start();                        /* the NVRAM answers from here: start this boot's record */
#endif
	BI->cs7_size = 0;
	BI->cs7_base = CS7_BASE;
	BI->cs7_virt = CS7_BASE;

	v = cfg32(cfg, CFG_OR4);                 /* main flash bank 1 on CS4, 16-bit */
	if (v == 0) {
		BR(4) = 0;
		OR(4) = 0;
		size1 = 0;
	} else {
		BI->flash1_base = FLASH1_BASE;
		BR(4) = FLASH1_BASE | 0x801;
		OR(4) = v;
		size1 = -(v & ~0x7FFFu);
	}
	BI->flash1_size = size1;
	BI->flash1_total = size1;

	v = cfg32(cfg, CFG_OR5);                 /* bank 2 on CS5, right after bank 1 */
	if (v == 0) {
		BR(5) = 0;
		OR(5) = 0;
		v = 0;
	} else {
		uint32_t base = FLASH1_BASE + size1;

		BI->flash2_base = base;
		BR(5) = base | 0x801;
		OR(5) = v;
		v = -(v & ~0x7FFFu);
	}
	BI->flash2_size = v;
	BI->flash2_total = v;

	SIUMCR = 0x00632000;
	PISCR = 0x82;
	MTSPR(SPR_ICTRL, 7);
	SCCR = (uint32_t)cfg16(cfg, CFG_SCCR) << 11;
	PLPRCR = pll_setup(cfg);

	wdt_kick();
	BR(2) = 0;
	BR(3) = 0;
	v = (uint32_t)cfg16(cfg, CFG_MAMR_INIT) << 16 | cfg16(cfg, CFG_MAMR_INIT + 2);
	MAMR = v;
	MBMR = v;

	while (i2c_read(0xA2, UPM_BUF, 256))     /* UPMA program from the second EEPROM */
		;
	wdt_kick();
	spin(0xFFFF);
	for (i = 0; i < 64; i++) {
		MDR = *(volatile uint32_t *)(UPM_BUF + 4 * i);
		MCR = i;
	}
	/* SDRAM init commands. The first one uses the whole word at 0x2C, whose low half is
	 * also the second command (as in the original). */
	v = cfg32(cfg, CFG_MCR_A) | 0x80000000;
	MCR = v | 0x4000;
	MCR = v | 0x6000;
	v = cfg16(cfg, CFG_MCR_B) | 0x80000000;
	MCR = v | 0x4000;
	MCR = v | 0x6000;
	MAR = cfg16(cfg, CFG_MAR);
	v = cfg16(cfg, CFG_MCR_C) | 0x80000000;
	MCR = v | 0x4000;
	MCR = v | 0x6000;
	MPTPR = (uint16_t)(cfg8(cfg, CFG_MPTPR) << 8);
	MAMR = (uint32_t)cfg16(cfg, CFG_MAMR_RUN) << 16 | cfg16(cfg, CFG_MAMR_RUN + 2);

	v = cfg16(cfg, CFG_SDRAM_OR) | 0xF8000000;  /* SDRAM: CS2 at 0, CS3 at 128 MB */
	OR(2) = v;
	OR(3) = v;
	BR(2) = 0x00000081;
	BR(3) = 0x08000081;

	BI->bank_list = (uint32_t *)BANK_LIST;
	for (i = 0; i < 17; i++)
		banks[i] = 0;
	for (i = 0; i < 8; i++)
		banks[i] = i * 0x02000000;
	banks[8] = 0xFFFFFFFF;

	code = cfg8(cfg, CFG_SDRAM_SIZE);
	if (code == 0) {
		BI->bank_size = 0x00800000;
		BI->sdram_total = 0x04000000;
	} else if (code == 1) {
		BI->bank_size = 0x01000000;
		BI->sdram_total = 0x08000000;
	} else {
		BI->bank_size = 0x02000000;
		BI->sdram_total = 0x10000000;
	}

#if defined(DUMP) && !defined(STAGE1)
	return boot_fixed_rest(cfg, stage0_try());   /* a stage 1 in the main flash takes over from here */
#else
	return boot_fixed_rest(cfg, 0);
#endif
}

/* The fixed path once the SDRAM works: the window, the load, the final MMU map. Stage 1 starts here (two stages,
 * serdump.h). window_done: the recovery window has been served already, no second one. */
int boot_fixed_rest(uintptr_t cfg, int window_done)
{
	(void)cfg;
	(void)window_done;
#ifdef DUMP
	/* SDRAM works and the caches are still off: catch our own exceptions from here (dbg.c), then the serial dump
	 * window first (serdump.c) */
	dbg_vectors_on();
	DBG_STEP(STEP_SDRAM, 0);
	if (!window_done)
		dump_window();
	DBG_STEP(STEP_WINDOW_DONE, 0);
#endif
#ifdef NET
	/* SDRAM works and the caches are still off: run the network window here. */
	struct arc_entry ne;
	int netimg = net_window(&ne);
#endif

	MTSPR(SPR_MI_CTR, 0);
	MTSPR(SPR_MD_CTR, 0);
	MTSPR(SPR_IC_CST, 0x0A000000); isync_sync();
	MTSPR(SPR_IC_CST, 0x0C000000); isync_sync();
	MTSPR(SPR_IC_CST, 0x02000000); isync_sync();
	MTSPR(SPR_DC_CST, 0x0A000000); isync_sync();
	MTSPR(SPR_DC_CST, 0x0C000000); isync_sync();
	MTSPR(SPR_DC_CST, 0x02000000); isync_sync();

#ifdef NET
	if (netimg)
		(void)load_image(&ne);
	else
#endif
#ifdef DUMP
	if (STAGE->magic == STAGE_MAGIC) {       /* run from RAM; if it does not load, boot as usual */
		DBG_STEP(STEP_STAGED, STAGE->phys);
		if (load_app_staged()) {
			STAGE->magic = 0;
			(void)load_app_fixed();
		}
	} else
#endif
	(void)load_app_fixed();                  /* the original ignores the result here */
	mmu_final_fixed(*(uint32_t **)0x4C, *(volatile uint32_t *)0x50);
	DBG_STEP(STEP_MMU, 0);
#ifdef PATCH
	apply_patches();                         /* real mode, data cache off: see patch.c */
#endif
	return 0;
}

/* SPD path. 0xF8180614..0xF8180720 */
static int boot_spd(uintptr_t cfg)
{
	int status;

	memmap_from_straps(cfg);
#ifdef DUMP
	dbg_boot_start();
#endif
	SIUMCR |= 0x00632000;
	PISCR = 0x82;
	MTSPR(SPR_ICTRL, 7);
	PLPRCR = pll_setup(cfg);

	while (sdram_from_spd(cfg))
		;
	wdt_kick();
#ifdef DUMP
	dump_window();
	STAGE->magic = 0;                        /* run from RAM is for the fixed-config path only */
#endif
#ifdef NET
	struct arc_entry ne;
	int netimg = net_window(&ne);
#endif
	mmu_init_spd();

#ifdef NET
	if (netimg ? load_image(&ne) == 0 : load_app_spd() == 0) {
#else
	if (load_app_spd() == 0) {
#endif
		status = 0;
		mmu_final(*(uint32_t **)0x4C, *(volatile uint32_t *)0x50);
#ifdef PATCH
		apply_patches();
#endif
	} else {
		status = 400;
		mmu_final(0, 0);
	}
	return status;
}

/* Boot info for the app at RAM 0x14..0x98. 0xF8180724..0xF8180834 */
static void handoff_info(uintptr_t cfg)
{
	volatile uint32_t *ram = 0;
	const uint32_t *banks = BI->bank_list;
	int i;

	ram[0x14 / 4] = BI->mapped_ram;
	ram[0x98 / 4] = cfg32(cfg, CFG_BOARD_ID);
	ram[0x1C / 4] = cfg32(cfg, CFG_OSCCLK);
	ram[0x20 / 4] = BI->sysclk;
	ram[0x24 / 4] = BI->nvram_virt;
	ram[0x28 / 4] = BI->nvram_size;
	ram[0x2C / 4] = BI->boot_virt;
	ram[0x30 / 4] = BI->boot_size;
	ram[0x34 / 4] = BI->cs7_base;
	ram[0x38 / 4] = BI->cs7_size;
	ram[0x3C / 4] = BI->flash1_base;
	ram[0x40 / 4] = BI->flash1_total;
	ram[0x44 / 4] = BI->flash2_base;
	ram[0x48 / 4] = BI->flash2_total;
	ram[0x54 / 4] = BI->bank_size;
	for (i = 0; i < 16; i++)
		ram[0x58 / 4 + i] = banks[i];
#ifdef DUMP
	{
		uint32_t slot = slot_where(0);

		if (slot) {                          /* the update slot and the loader sector are never part of the
		                                      * app's download volume */
			if (BI->flash2_size)
				ram[0x48 / 4] = BI->flash2_total - SLOT_SIZE - LOADER_SIZE;
			else
				ram[0x40 / 4] = BI->flash1_total - SLOT_SIZE - LOADER_SIZE;
		}
	}
	if (STAGE->magic == STAGE_MAGIC) {       /* the staged image is the app's boot flash, and not its RAM */
		ram[0x2C / 4] = STAGE->virt;
		ram[0x30 / 4] = STAGE_SIZE;
		ram[0x14 / 4] = BI->mapped_ram - STAGE_SIZE;
	}
	if ((BOOT_FLAGS & SLOT_FLAG) && (dbg_options() & DBG_OPT_SLOT_DIRECT) && STAGE->magic != STAGE_MAGIC &&
	    slot_where(0)) {
		ram[0x2C / 4] = slot_where(0);       /* debugging: booted from the slot in place */
		ram[0x30 / 4] = SLOT_SIZE;
	}
	DBG_STEP(STEP_HANDOFF, ram[0x2C / 4]);
#endif
}

#ifdef STAGE1
/* Stage 1's entry (start.S _s1_entry, from stage 0's stage0_try): the clocks, chip selects, SDRAM, I2C and the timer
 * are set up, the config EEPROM is in CFG_BUF, the boot info in DPRAM. Never returns. */
void stage1_main(uintptr_t cfg)
{
	int status;

	DBG_STEP(STEP_STAGE1, S1_LOAD);
	status = boot_fixed_rest(cfg, 0);
	handoff_info(cfg);
	i2c_stop();
	dbg_set_s1_tries(0);                     /* stage 1 worked: stage 0 keeps using it */
	boot_handoff(status);
}
#endif

void boot_main(void)
{
	uintptr_t cfg = CFG_BUF;
	volatile uint32_t *p;
	int status;

	SYPCR = 0xFFFFFF8F;                      /* watchdog on, ~2.7 s at 50 MHz */
	MTSPR(SPR_DER, 0x73E67C0F);
	TBSCRK = 0x55CCAA33;                     /* unlock the keyed registers */
	TBREFAK = 0x55CCAA33;
	SCCR = 0;

	for (p = (volatile uint32_t *)BOOTINFO; p < (volatile uint32_t *)(BOOTINFO + 0x58); p++)
		*p = 0;
	BI->boot_base = BOOT_BASE;
	BI->boot_size = BOOT_SIZE;
#ifdef DUMP
	STAGE->magic = 0;
#endif
	BR(0) = BOOT_BASE | 0x401;
	OR(0) = ~(BOOT_SIZE - 1) | 0x950;

	timer_init();
	i2c_init();
	read_cfg(cfg);

	if (cfg8(cfg, CFG_MEM_TYPE) == 1 && cfg8(cfg, CFG_MEM_SUB) < 0x50)
		status = boot_spd(cfg);
	else
		status = boot_fixed(cfg);

	handoff_info(cfg);
	i2c_stop();
	boot_handoff(status);
}
