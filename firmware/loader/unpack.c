/* NRV2D, the smallest decoder: the firmware extension's code travels NRV2D-packed in the boot flash's EXT entry
 * (analysis/tools/fwext.py, bootloader/tools/nrv2d_pack.py), and the extension loader (fwext.py loader_asm) calls this
 * at boot_card time to unpack it into its RAM block, before checking its tag. The stream is our own packer's, checked
 * by that tag afterwards, so there are no bounds checks here: the same decoding as bootloader/src/nrv2d.c (the stock
 * bootstrap's NRV2D, 8-bit bit buffer), in as few instructions as possible, to fit a free gap of the app. Returns
 * the unpacked length. */
#include <stdint.h>

uint32_t ext_unpack(const uint8_t *src, uint8_t *dst)
{
	uint32_t bb = 0, ip = 0, op = 0, last = 1, off, len;
#define BIT() ((bb <<= 1, (bb & 0xFF) ? 0 : (bb = (uint32_t)src[ip++] * 2u + 1u)), (bb >> 8) & 1u)

	for (;;) {
		if (BIT()) {                                /* a literal */
			dst[op++] = src[ip++];
			continue;
		}
		off = 1;
		for (;;) {
			off = off * 2 + BIT();
			if (BIT())
				break;
			off = (off - 1) * 2 + BIT();
		}
		if (off == 2) {
			off = last;
			len = BIT();
		} else {
			off = (off - 3) * 256 + src[ip++];
			if (off == 0xFFFFFFFFu)                 /* the end */
				return op;
			len = ~off & 1;
			off = (off >> 1) + 1;
			last = off;
		}
		len = len * 2 + BIT();
		if (!len) {
			len = 1;
			do
				len = len * 2 + BIT();
			while (!BIT());
			len += 2;
		}
		if (off > 0x500)
			len++;
		const uint8_t *p = dst + op - off;
		len++;
		do
			dst[op++] = *p++;
		while (--len);
	}
}
