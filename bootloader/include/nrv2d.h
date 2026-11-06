/* UCL NRV2D decompressor, as implemented in BOOTSTRAP V2.00 (0xF8180C90).
 * Portable: also builds on the host for testing. */
#ifndef NRV2D_H
#define NRV2D_H

#include <stdint.h>

/* Called once per decoded token. The target build kicks the watchdog here. */
#ifndef NRV2D_POLL
#define NRV2D_POLL() ((void)0)
#endif

/* Decompress src[0..src_len) into dst.
 * Fails if a match reaches back before dst, or if the output pointer reaches dst_limit
 * (the original passes the staging buffer as the limit, so output may not overrun its input).
 * Returns 0 on success, 1 on error. *out_len (may be NULL) receives the bytes written. */
int nrv2d_decompress(const uint8_t *src, uint32_t src_len, uint8_t *dst,
                     const uint8_t *dst_limit, uint32_t *out_len);

#endif
