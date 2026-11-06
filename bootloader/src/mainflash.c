/* The update slot: the last SLOT_SIZE bytes of the main flash hold a whole boot-flash image installed over the DB9
 * (serial window INSTALL). The boot loader boots its app first, counting tries in NVRAM as for the original's
 * upgrade images; the app clears the count once it runs (nvram_downloaded_boot_clear after 1200 panel frames), and
 * after failed tries the boot goes back to the boot flash. The boot flash itself is never written here.
 *
 * Main flash: AMD AM29LV128-class parts on 16-bit chip selects (CS4, CS5), command addresses in words relative to
 * the chip's base. Uniform 64 KB sectors are assumed; with bigger sectors an erase inside the slot erases the whole
 * sector, which still lies inside the 512 KB-aligned slot. */
#include "board.h"
#include "serdump.h"

#define MF16(a) (*(volatile uint16_t *)(a))
#define SECTOR  0x10000u

/* The slot: the end of flash bank 2, else of bank 1. Its chip's base in *chip. 0 if there is no main flash. */
uint32_t slot_where(uint32_t *chip)
{
	uint32_t base, size;

	if (BI->flash2_size) {
		base = BI->flash2_base;
		size = BI->flash2_size;
	} else {
		base = BI->flash1_base;
		size = BI->flash1_size;
	}
	if (size < 2 * SLOT_SIZE + LOADER_SIZE)
		return 0;
	if (chip)
		*chip = base;
	return base + size - SLOT_SIZE;
}

static void unlock(uint32_t chip)
{
	MF16(chip + 2 * 0x555) = 0xAA;
	MF16(chip + 2 * 0x2AA) = 0x55;
}

/* Where the last flash operation failed is kept in flash_fail (DPRAM) for the window's error reply. */
static int fail(uint32_t chip, uint8_t step, uint32_t a, uint16_t got, uint16_t want)
{
	flash_fail.step = step;
	flash_fail.addr = a;
	flash_fail.got = got;
	flash_fail.want = want;
	MF16(chip) = 0xF0;                                 /* back to read mode */
	return -1;
}

/* Wait for an embedded operation as the original's own driver does (flash_program_type_b): erase ends when the
 * word reads 0xFFFF, a program when DQ7 reads back the written bit 7. Only DQ7 is defined during status reads;
 * the upper byte is not (word mode), so DQ5/DQ13 are not used: a stuck operation runs into the limit instead. */
static int wait_done(uint32_t a, uint16_t v, int erase, uint32_t limit, uint16_t *last)
{
	uint32_t n;
	uint16_t s = 0;

	for (n = 0; n < limit; n++) {
		s = MF16(a);
		if (erase ? s == 0xFFFF : !((s ^ v) & 0x80))
			break;
		if (!(n & 0xFFF))
			wdt_kick();
	}
	s = MF16(a);                                       /* the settled word, read once more */
	*last = s;
	return s == v ? 0 : -1;
}

int slot_erase_sector(uint32_t chip, uint32_t sector)
{
	uint16_t s;

	MF16(chip) = 0xF0;
	unlock(chip);
	MF16(chip + 2 * 0x555) = 0x80;
	unlock(chip);
	MF16(sector) = 0x30;
	if (wait_done(sector, 0xFFFF, 1, 400000000u, &s))  /* tens of seconds, the original allows 60 */
		return fail(chip, FLASH_STEP_ERASE, sector, s, 0xFFFF);
	return 0;
}

static int program_word(uint32_t chip, uint32_t a, uint16_t v)
{
	uint16_t s;

	if (v == 0xFFFF) {
		s = MF16(a);
		return s == 0xFFFF ? 0 : fail(chip, FLASH_STEP_BLANK, a, s, v);
	}
	unlock(chip);
	MF16(chip + 2 * 0x555) = 0xA0;
	MF16(a) = v;
	if (wait_done(a, v, 0, 4000000u, &s))
		return fail(chip, FLASH_STEP_PROGRAM, a, s, v);
	return 0;
}

/* Erase [dst, dst + size) and program n bytes from src into it. 0, or -1 on a flash error. */
static int program_region(uint32_t chip, uint32_t dst, uint32_t size, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	flash_fail.step = 0;
	if (!dst || n > size) {
		flash_fail.step = FLASH_STEP_NONE;
		return -1;
	}
	for (i = 0; i < size; i += SECTOR) {
		wdt_kick();
		if (slot_erase_sector(chip, dst + i))
			return -1;
	}
	for (i = 0; i < n; i += 2) {
		uint16_t v = (uint16_t)(src[i] << 8 | (i + 1 < n ? src[i + 1] : 0xFF));

		if (program_word(chip, dst + i, v))
			return -1;
		if (!(i & 0x3FFF))
			wdt_kick();
	}
	return 0;
}

int slot_install(const uint8_t *src, uint32_t n)
{
	uint32_t chip, slot = slot_where(&chip);

	return program_region(chip, slot, SLOT_SIZE, src, n);
}

/* Make the slot unbootable: erase its first sector (the boot loader and the archive start there). */
int slot_uninstall(void)
{
	uint32_t chip, slot = slot_where(&chip);

	flash_fail.step = 0;
	return slot ? slot_erase_sector(chip, slot) : -1;
}

/* The loader sector (two stages, serdump.h): the 64 KB just below the slot. */
uint32_t loader_where(void)
{
	uint32_t slot = slot_where(0);

	return slot ? slot - LOADER_SIZE : 0;
}

int loader_install(const uint8_t *src, uint32_t n)
{
	uint32_t chip;

	(void)slot_where(&chip);
	return program_region(chip, loader_where(), LOADER_SIZE, src, n);
}

int loader_uninstall(void)
{
	uint32_t chip, a = loader_where();

	(void)slot_where(&chip);
	flash_fail.step = 0;
	return a ? slot_erase_sector(chip, a) : -1;
}

/* ---- the boot flash (CS0, the chip itself): 8-bit AMD AM29LV040-class, 8 sectors of 64 KB ----
 *
 * Writing the chip we boot from is only safe when the running code is NOT in it: as stage 1 (run from SDRAM) or any
 * RUN-from-RAM boot loader, never as stage 0 in place. running_from_bootflash() refuses otherwise, so a build in
 * the chip can never erase itself. The window (serdump.c) is single-threaded with interrupts off and nothing else
 * touches the chip, unlike the firmware, where the panel task reads its archive from the boot volume. */
#define MF8(a)  (*(volatile uint8_t *)(a))

int running_from_bootflash(void)
{
	uint32_t here = (uint32_t)(uintptr_t)&running_from_bootflash;

	return here >= BOOT_BASE && here < BOOT_BASE + 0x800000u;
}

static void bf_unlock(uint32_t chip)
{
	MF8(chip + 0x555) = 0xAA;
	MF8(chip + 0x2AA) = 0x55;
}

static int bf_wait(uint32_t a, uint8_t v, int erase, uint32_t limit, uint8_t *last)
{
	uint32_t n;
	uint8_t s = 0;

	for (n = 0; n < limit; n++) {
		s = MF8(a);
		if (erase ? s == 0xFF : !((s ^ v) & 0x80))
			break;
		if (!(n & 0xFFF))
			wdt_kick();
	}
	s = MF8(a);
	*last = s;
	return s == v ? 0 : -1;
}

/* The chip's manufacturer and device id (autoselect), for the window to show that writes reach it. */
uint32_t boot_flash_id(void)
{
	uint32_t chip = BOOT_BASE;
	uint8_t man, dev;

	bf_unlock(chip);
	MF8(chip + 0x555) = 0x90;
	man = MF8(chip + 0);
	dev = MF8(chip + 1);
	MF8(chip) = 0xF0;
	return (uint32_t)man << 8 | dev;
}

/* Erase the boot flash and program n bytes from src into it. 0, or -1 (flash_fail set) on error or if refused. */
int boot_flash_install(const uint8_t *src, uint32_t n)
{
	uint32_t chip = BOOT_BASE, i;
	uint8_t s;

	flash_fail.step = 0;
	if (running_from_bootflash() || n > BOOT_SIZE) {
		flash_fail.step = FLASH_STEP_NONE;
		return -1;
	}
	for (i = 0; i < BOOT_SIZE; i += SECTOR) {
		wdt_kick();
		MF8(chip) = 0xF0;
		bf_unlock(chip);
		MF8(chip + 0x555) = 0x80;
		bf_unlock(chip);
		MF8(chip + i) = 0x30;
		if (bf_wait(chip + i, 0xFF, 1, 400000000u, &s)) {
			flash_fail.step = FLASH_STEP_ERASE;
			flash_fail.addr = chip + i;
			flash_fail.got = s;
			flash_fail.want = 0xFF;
			MF8(chip) = 0xF0;
			return -1;
		}
	}
	for (i = 0; i < n; i++) {
		uint8_t v = src[i];

		if (v != 0xFF) {
			bf_unlock(chip);
			MF8(chip + 0x555) = 0xA0;
			MF8(chip + i) = v;
			if (bf_wait(chip + i, v, 0, 4000000u, &s)) {
				flash_fail.step = FLASH_STEP_PROGRAM;
				flash_fail.addr = chip + i;
				flash_fail.got = s;
				flash_fail.want = v;
				MF8(chip) = 0xF0;
				return -1;
			}
		}
		if (!(i & 0x3FFF))
			wdt_kick();
	}
	return 0;
}
