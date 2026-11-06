/* Boot flash archive ("LFS" image) as walked by BOOTSTRAP V2.00 (0xF8181EA8).
 * Portable: also builds on the host for testing.
 *
 * The flash is a chain of 4-byte-aligned entries, terminated by a zero length word:
 *   +0  u32  entry length, including the trailing check word
 *   +4  u32  uncompressed size (the bootstrap entry stores length-4 here)
 *   +8  u32  flags
 *   +12 u8   name length, then the name; data starts at (namelen + 16) & ~3
 *   end u32  check word: CRC-32 of [0, len-4), except for bootable entries (flag bit 0),
 *            which carry (sum16 << 16) | xor16 over the 16-bit halves of [0, len-4).
 * The bootstrap itself is entry 0 ("BOOTSTRAP V2.00").
 * A region may continue in a second "wrap" region (two flash banks seen as one).
 */
#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <stdint.h>

#define ARC_BOOTABLE  0x00000001u
#define ARC_NRV2D     0x20000000u
#define ARC_ZLIB      0x40000000u

struct arc_entry {
	uintptr_t data;       /* first data byte */
	uint32_t  len;        /* data bytes, excluding the check word */
	uint32_t  flags;
	uintptr_t end;        /* end of the region data lives in */
	uintptr_t wrap_base;  /* continuation region, 0 if none */
	uintptr_t wrap_end;
};

/* Big-endian word load (a plain load on the target). */
static inline uint32_t arc_rd32(uintptr_t a)
{
#if defined(__powerpc__) || defined(__PPC__)
	return *(volatile const uint32_t *)a;
#else
	const uint8_t *p = (const uint8_t *)a;
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
#endif
}

/* Sum/xor check word used for bootable entries. */
uint32_t arc_boot_check(uintptr_t entry, uint32_t len_minus_4);

/* Standard CRC-32 (zlib), bitwise: used for the check word of ordinary entries. */
uint32_t arc_crc32(uintptr_t data, uint32_t len);

/* Find an entry by name (any flags) whose CRC-32 check word is valid; no wrap region.
 * Returns 1 and fills *e (data, len, flags) on success. */
int arc_find_named(uintptr_t base, uint32_t len, const char *name, struct arc_entry *e);

/* Find the first bootable entry with a valid check word.
 * Returns 1 and fills *e on success, 0 if none. */
int arc_find_boot(uintptr_t base, uint32_t len, uintptr_t wrap_base, uint32_t wrap_len,
                  struct arc_entry *e);

#endif
