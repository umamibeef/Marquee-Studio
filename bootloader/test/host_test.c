/* Host test: walk the real boot flash dump the way the bootstrap does, decompress the
 * bootable entry with the reconstructed NRV2D, and compare with the known-good ELITE_RGB.
 * Usage: host_test bootflash.bin ELITE_RGB */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "archive.h"
#include "nrv2d.h"

static uint8_t *slurp(const char *path, long *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;

	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
	b = malloc(*n);
	if (fread(b, 1, *n, f) != (size_t)*n) { perror(path); exit(2); }
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	long flen, rlen;
	uint8_t *flash, *ref, *out;
	struct arc_entry e;
	uint32_t got = 0;
	int fails = 0;

	if (argc != 3) { fprintf(stderr, "usage: %s bootflash.bin ELITE_RGB\n", argv[0]); return 2; }
	flash = slurp(argv[1], &flen);
	ref = slurp(argv[2], &rlen);

	if (!arc_find_boot((uintptr_t)flash, (uint32_t)flen, 0, 0, &e)) {
		printf("FAIL find: no bootable entry\n");
		return 1;
	}
	printf("found bootable entry: data at +0x%lx, %u bytes, flags 0x%08x\n",
	       (unsigned long)(e.data - (uintptr_t)flash), e.len, e.flags);
	if (e.flags != (ARC_NRV2D | ARC_BOOTABLE)) { printf("FAIL flags\n"); fails++; }

	/* dst limit = a generous buffer end (on the board it is the 0x300000 staging area) */
	out = calloc(1, 0x300000);
	if (nrv2d_decompress((const uint8_t *)e.data, e.len, out, out + 0x300000, &got)) {
		printf("FAIL nrv2d returned error after %u bytes\n", got);
		fails++;
	} else if (got != (uint32_t)rlen || memcmp(out, ref, rlen)) {
		printf("FAIL output mismatch: %u bytes vs %ld reference\n", got, rlen);
		fails++;
	} else {
		printf("PASS: NRV2D output = ELITE_RGB, %u bytes identical\n", got);
	}

	/* negative checks: a corrupted check word must make the entry invisible */
	{
		uint8_t *bad = malloc(flen);
		uintptr_t off = e.data - (uintptr_t)flash + e.len; /* check word follows the data */
		memcpy(bad, flash, flen);
		bad[off] ^= 0x01;
		if (arc_find_boot((uintptr_t)bad, (uint32_t)flen, 0, 0, &e)) {
			printf("FAIL: corrupted check word still accepted\n"); fails++;
		} else printf("PASS: corrupted check word rejected\n");
		free(bad);
	}
	/* truncated stream must fail cleanly */
	arc_find_boot((uintptr_t)flash, (uint32_t)flen, 0, 0, &e);
	if (nrv2d_decompress((const uint8_t *)e.data, e.len / 2, out, out + 0x300000, &got) == 0) {
		printf("FAIL: truncated stream accepted\n"); fails++;
	} else printf("PASS: truncated stream rejected after %u bytes\n", got);

	printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", fails);
	return fails != 0;
}
