/* Apply the "PATCH" archive entry to the application in RAM (built with make PATCH=1).
 *
 * Runs after the final MMU set-up, when the bootloader is back in real mode with the data cache off,
 * so every write goes straight to SDRAM; boot_handoff then invalidates and enables the caches.
 * All-or-nothing: if any expected word differs (another firmware version), nothing is applied.
 * Payload format: see tools/mkpatch.py. */
#include "board.h"
#include "archive.h"

#define MKPT 0x4D4B5054u       /* 'MKPT' */

/* Returns the number of word patches applied (0 if none or no PATCH entry). */
int apply_patches(void)
{
	struct arc_entry e;
	uintptr_t p, code;
	uint32_t load, clen, n, i;

	if (!arc_find_named(BI->boot_base, BI->boot_size, "PATCH", &e) || e.len < 20)
		return 0;
	p = e.data;
	if (arc_rd32(p) != MKPT || arc_rd32(p + 4) != 1)
		return 0;
	load = arc_rd32(p + 8);
	clen = arc_rd32(p + 12);
	n = arc_rd32(p + 16);
	code = p + 20 + n * 12;
	if (n > 64 || 20 + n * 12 + clen > e.len)
		return 0;

	for (i = 0; i < n; i++) {                      /* check everything first */
		uintptr_t a = arc_rd32(p + 20 + i * 12);
		if (*(volatile uint32_t *)a != arc_rd32(p + 24 + i * 12))
			return 0;
	}
	for (i = 0; i < clen; i++)
		*(volatile uint8_t *)(load + i) = *(volatile const uint8_t *)(code + i);
	for (i = 0; i < n; i++)
		*(volatile uint32_t *)(uintptr_t)arc_rd32(p + 20 + i * 12) = arc_rd32(p + 28 + i * 12);
	return (int)n;
}
