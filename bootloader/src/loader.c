/* Application loader. Translated from 0xF8181C18 (fixed-config path), 0xF8181CF8 (SPD path)
 * and their shared copy/decompress tail at 0xF8181E04.
 *
 * Boot order:
 *  1. Unless BOOT_TRIES is 0xFF, look for a bootable image in the main flash (an upgrade
 *     downloaded by the app). Each attempt increments BOOT_TRIES. The 4th attempt falls back
 *     to the boot flash, and past that the counter parks at 0xFF (upgrade disabled).
 *     BOOT_FLAGS bit 0 tells the app which image it was started from.
 *  2. Otherwise, or if the upgrade fails to decompress, boot the image in the boot flash.
 * Images flagged NRV2D are copied to APP_STAGE and decompressed to address 0;
 * others are copied straight to address 0. */
#include "board.h"
#include "archive.h"
#include "nrv2d.h"
#ifdef DUMP
#include "serdump.h"
#endif

/* Bump the attempt counter. Returns nonzero to use the upgrade image. */
static int count_try(void)
{
	uint32_t n = BOOT_TRIES + 1u;

	BOOT_FLAGS |= 1;
	BOOT_TRIES = (uint8_t)(n > 4 ? 0xFF : n);
	return n != 4;
}

#ifdef DUMP
/* A try of the update slot: nonzero to boot it. After 3 tries that did not reach 1200 frames, never again. */
static int slot_try(void)
{
	uint32_t n = BOOT_TRIES + 1u;

	if (n > 3) {
		BOOT_TRIES = 0xFF;
		dbg_flush(&BOOT_TRIES, 2);
		return 0;
	}
	BOOT_TRIES = (uint8_t)n;
	BOOT_FLAGS = (uint8_t)((BOOT_FLAGS & ~1u) | SLOT_FLAG);
	dbg_flush(&BOOT_TRIES, 2);                       /* now, not at mmu_final: a crash before it must count */
	return 1;
}

/* Copy the slot into the stage and mark it for the hand-off as a staged boot. 0, or -1 without a stage. */
static int slot_to_stage(uint32_t slot)
{
	uint32_t phys, virt, i;

	stage_where(&phys, &virt);
	if (!phys)
		return -1;
	for (i = 0; i < SLOT_SIZE; i += 4) {
		*(volatile uint32_t *)(phys + i) = *(volatile const uint32_t *)(slot + i);
		if (!(i & 0xFFFF))
			wdt_kick();
	}
	STAGE->phys = phys;
	STAGE->virt = virt;
	STAGE->len = SLOT_SIZE;
	STAGE->magic = STAGE_MAGIC;
	return 0;
}
#endif

/* 0xF8181E04. Returns 0 on success, 400 on a bad wrap, 1 if decompression failed. */
int load_image(const struct arc_entry *e)
{
	uintptr_t dst = (e->flags & ARC_NRV2D) ? APP_STAGE : APP_LOAD;
	uintptr_t s = e->data, end = e->end, wbase = e->wrap_base, wend = e->wrap_end;
	uint32_t i;

	DBG_STEP(STEP_LOADING, (uint32_t)s);
#ifdef DUMP
	if (dst == APP_LOAD)
		dbg_vectors_off();                   /* address 0 is about to be overwritten (dbg.c) */
#endif
	for (i = 0; i < e->len; i += 4) {
		if (s >= end) {                      /* continue in the second bank */
			s -= end;
			if (wbase == 0)
				return 400;
			s += wbase;
			end = wend;
			wbase = 0;
			wend = 0;
		}
		*(volatile uint32_t *)(dst + i) = *(volatile const uint32_t *)s;
		s += 4;
	}

	if (e->flags & ARC_NRV2D) {
#ifdef DUMP
		dbg_vectors_off();
#endif
		if (nrv2d_decompress((const uint8_t *)APP_STAGE, e->len, (uint8_t *)APP_LOAD,
		                     (const uint8_t *)APP_STAGE, 0))
			return 1;
	}
	DBG_STEP(STEP_LOADED, 0);
	return 0;
}

/* Load the boot flash image, starting with an already-found upgrade if have_upgrade. */
static int load_loop(struct arc_entry *e, int have_upgrade)
{
	for (;;) {
		int rc;

		if (!have_upgrade) {
			BOOT_FLAGS &= ~1;
			if (!arc_find_boot(BI->boot_base, BI->boot_size, 0, 0, e))
				return 400;
		}
		have_upgrade = 0;
		rc = load_image(e);
		if (rc != 1)
			return rc;
		/* decompression failed: retry from the boot flash */
	}
}

int load_app_fixed(void)
{
	struct arc_entry e;

	BOOT_FLAGS &= ~1;
#ifdef DUMP
	/* Our boot loader: the upgrade is the image in the update slot (mainflash.c, installed over the DB9). It is
	 * copied to the stage in SDRAM and booted from there exactly as a RUN from RAM (the path proven on the sign),
	 * so the app sees its boot volume in SDRAM. Tries are counted in BOOT_TRIES with our own BOOT_FLAGS bit 1 (the
	 * original's bit 0 makes the app look for its image in the main flash): the firmware clears both after 1200
	 * panel frames (firmware/ext/slotboot.c). The 4th try parks the counter at 0xFF and boots the boot flash. */
	if (BOOT_TRIES != 0xFF) {
		uint32_t slot = slot_where(0);

		if (slot && arc_find_boot(slot, SLOT_SIZE, 0, 0, &e)) {
			DBG_STEP(STEP_SLOT_FOUND, slot);
			if (slot_try()) {
				DBG_STEP(STEP_SLOT_TRY, BOOT_TRIES);
				if (dbg_options() & DBG_OPT_SLOT_DIRECT) {
					/* debugging: in place, the main flash as the boot volume (main.c), with the
					 * original's bit 0 as well, as the first slot boots on the sign did */
					BOOT_FLAGS |= 1;
					DBG_STEP(STEP_SLOT_DIRECT, slot);
					return load_loop(&e, 1);
				}
				if (!slot_to_stage(slot)) {
					DBG_STEP(STEP_SLOT_COPIED, STAGE->phys);
					if (load_app_staged() == 0)
						return 0;
					STAGE->magic = 0;
				}
			}
		}
	}
	BOOT_FLAGS &= ~SLOT_FLAG;
	DBG_STEP(STEP_CHIP, 0);
	return load_loop(&e, 0);
#else
	int up = 0;

	if (BOOT_TRIES != 0xFF) {
		uint32_t base = BI->flash1_base, size = BI->flash1_size;

		if (size == 0)
			base = BI->flash2_base;
		size += BI->flash2_size;
		if (size != 0 && arc_find_boot(base, size, 0, 0, &e))
			up = count_try();
	}
	return load_loop(&e, up);
#endif
}

int load_app_spd(void)
{
	struct arc_entry e;
	uint32_t wb = 0, wl = 0;
	int up = 0, found = 0;

	BOOT_FLAGS &= ~1;
	if (BOOT_TRIES != 0xFF) {
		if (BI->flash1_size) {
			/* bank 1 continues into bank 2, or else into CS7 */
			wl = BI->flash2_size;
			if (wl) {
				wb = BI->flash2_base;
			} else {
				wl = BI->cs7_size;
				if (wl)
					wb = BI->cs7_base;
			}
			found = arc_find_boot(BI->flash1_base, BI->flash1_size, wb, wl, &e);
		}
		if (!found && BI->cs7_size)
			found = arc_find_boot(BI->cs7_base, BI->cs7_size, wb, wl, &e);
		if (found)
			up = count_try();
	}
	return load_loop(&e, up);
}

#ifdef DUMP
/* Run from RAM: the app from the image the serial window staged (RUN checked its CRC and that it has a bootable
 * entry). No NVRAM is touched: the boot counter and flags stay as they are. */
int load_app_staged(void)
{
	struct arc_entry e;

	if (!arc_find_boot(STAGE->phys, STAGE_SIZE, 0, 0, &e))
		return 400;
	return load_image(&e);
}
#endif
