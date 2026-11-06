/* Archive walk, translated from BOOTSTRAP V2.00 (0xF8181EA8..0xF8182044).
 * Register map of the original: r8 base, r9 length, r20/r21 wrap base/length,
 * r10 cursor, r14 region end, r22 bytes left, r11 entry length.
 * Outputs: r3 data, r5 data length, r6 flags, r14/r15/r16 copy-time wrap state. */
#include "archive.h"

uint32_t arc_boot_check(uintptr_t entry, uint32_t len_minus_4)
{
	uint32_t sum = 0, xr = 0, i;

	for (i = 0; i < len_minus_4; i += 4) {
		uint32_t w = arc_rd32(entry + i);
		uint32_t hi = w >> 16, lo = w & 0xFFFF;
		sum += hi + lo;
		xr ^= hi ^ lo;
	}
	return (sum << 16) | xr;
}

int arc_find_boot(uintptr_t base, uint32_t len, uintptr_t wrap_base, uint32_t wrap_len,
                  struct arc_entry *e)
{
	uintptr_t p = base;
	uintptr_t end = base + len;
	uint32_t left = len + wrap_len;

	for (;;) {
		uint32_t elen = arc_rd32(p);    /* the length word itself is never wrap-checked */
		uint32_t namelen, flags;

		if (elen == 0 || elen > left)
			return 0;

		/* The original bounds-checks +13 and +12 against the region end. In the wrap case it
		 * computes the wrapped address and then falls through to a plain load anyway, so the
		 * name length always comes from p+12. Same for the flags word at p+8. */
		if (p + 13 >= end && wrap_len == 0)
			return 0;
		if (p + 12 >= end && wrap_len == 0)
			return 0;
		namelen = *(volatile const uint8_t *)(p + 12);
		if (p + 8 >= end && wrap_len == 0)
			return 0;
		flags = arc_rd32(p + 8);

		if ((flags & ARC_BOOTABLE) &&
		    arc_rd32(p + elen - 4) == arc_boot_check(p, elen - 4)) {
			uint32_t hdr = (namelen + 16) & ~3u;
			uintptr_t d = p + hdr;

			if (d >= end) {
				if (wrap_len == 0)
					return 0;
				d = d - end + wrap_base;
				end = wrap_base + wrap_len;
				wrap_base = 0;
				wrap_len = 0;
			}
			e->data = d;
			e->len = elen - hdr - 4;
			e->flags = flags;
			e->end = end;
			e->wrap_base = wrap_base;
			e->wrap_end = wrap_base + wrap_len;
			return 1;
		}

		/* next entry */
		if (p + elen >= end) {
			if (wrap_len == 0)
				return 0;
			/* As in the original, the entry length is added again after wrapping. */
			p = p + elen - end + wrap_base;
			end = wrap_base + wrap_len;
			wrap_base = 0;
			wrap_len = 0;
		}
		p = (p + elen + 3) & ~(uintptr_t)3;
		left -= elen;
	}
}

uint32_t arc_crc32(uintptr_t data, uint32_t len)
{
	uint32_t crc = 0xFFFFFFFFu, i;
	int k;

	for (i = 0; i < len; i++) {
		crc ^= *(volatile const uint8_t *)(data + i);
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
	}
	return ~crc;
}

int arc_find_named(uintptr_t base, uint32_t len, const char *name, struct arc_entry *e)
{
	uintptr_t p = base, end = base + len;

	while (p + 16 <= end) {
		uint32_t elen = arc_rd32(p);
		uint32_t namelen, hdr, i;

		if (elen == 0 || elen == 0xFFFFFFFFu || p + elen > end)
			return 0;
		namelen = *(volatile const uint8_t *)(p + 12);
		for (i = 0; i < namelen && name[i] == *(volatile const char *)(p + 13 + i); i++)
			;
		if (i == namelen && name[i] == 0 && arc_rd32(p + elen - 4) == arc_crc32(p, elen - 4)) {
			hdr = (namelen + 16) & ~3u;
			e->data = p + hdr;
			e->len = elen - hdr - 4;
			e->flags = arc_rd32(p + 8);
			e->end = end;
			e->wrap_base = 0;
			e->wrap_end = 0;
			return 1;
		}
		p = (p + elen + 3) & ~(uintptr_t)3;
	}
	return 0;
}
