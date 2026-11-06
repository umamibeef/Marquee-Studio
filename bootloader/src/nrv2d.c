/* UCL NRV2D decompressor, translated from BOOTSTRAP V2.00 (0xF8180C90..0xF8180E44).
 *
 * The original is hand-written assembly with an 8-bit bit buffer (r5), input index r6,
 * output index r7, last match offset r9, input length r19, input base r20, output base r21.
 * Differences from the stock UCL decoder are kept on purpose:
 *  - the output may not reach the input base (dst_limit);
 *  - the stream may end up to 3 bytes before src_len (word-padded images).
 */
#include "nrv2d.h"

struct bitin {
	const uint8_t *src;
	uint32_t len;
	uint32_t ip;
	uint32_t bb;    /* bit buffer: byte << 1 | sentinel */
	int err;
};

/* 0xF8180CB0 */
static inline uint32_t getbit(struct bitin *b)
{
	b->bb <<= 1;
	if ((b->bb & 0xFF) == 0) {
		if (b->ip >= b->len) {
			/* The original reads one byte past the end, then aborts the whole decode.
			 * Returning 1 makes every caller loop terminate; err is checked after. */
			b->err = 1;
			return 1;
		}
		b->bb = (uint32_t)b->src[b->ip++] * 2 + 1;
	}
	return (b->bb >> 8) & 1;
}

static inline uint32_t rotl(uint32_t v, unsigned n)
{
	return (v << n) | (v >> (32 - n));
}

int nrv2d_decompress(const uint8_t *src, uint32_t src_len, uint8_t *dst,
                     const uint8_t *dst_limit, uint32_t *out_len)
{
	struct bitin b = { src, src_len, 0, 0, 0 };
	uint32_t op = 0, last_m_off = 1, m_off, m_len;
	int rc = 1;

	for (;;) {
		NRV2D_POLL();
		if (getbit(&b)) {
			if (b.err)
				goto out;
			/* literal */
			if (b.ip >= b.len)
				goto out;
			dst[op++] = src[b.ip++];
			continue;
		}

		m_off = 1;
		for (;;) {
			m_off = rotl(m_off, 1) + getbit(&b);
			if (getbit(&b))
				break;
			m_off = rotl(m_off - 1, 1) + getbit(&b);
		}
		if (b.err)
			goto out;

		if (m_off == 2) {
			m_off = last_m_off;
			m_len = getbit(&b);
		} else {
			if (b.ip >= b.len)
				goto out;
			m_off = rotl(m_off - 3, 8) + src[b.ip++];
			if (m_off == 0xFFFFFFFFu) {
				/* end marker: accept up to 3 bytes of padding */
				uint32_t rest = src_len - b.ip;
				rc = rest <= 3 ? 0 : 1;
				goto out;
			}
			m_len = (~m_off) & 1;
			m_off = (m_off >> 1) + 1;
			last_m_off = m_off;
		}

		m_len = m_len * 2 + getbit(&b);
		if (m_len == 0) {
			m_len = 1;
			do
				m_len = m_len * 2 + getbit(&b);
			while (!getbit(&b));
			m_len += 2;
		}
		if (b.err)
			goto out;
		if ((int32_t)m_off > 0x500)
			m_len++;

		{
			const uint8_t *m_pos = dst + op - m_off;
			uint32_t n = m_len + 1;

			if ((uintptr_t)m_pos < (uintptr_t)dst)
				goto out;
			do {
				dst[op++] = *m_pos++;
				if ((uintptr_t)(dst + op) >= (uintptr_t)dst_limit)
					goto out;
			} while (--n);
		}
	}
out:
	if (out_len)
		*out_len = op;
	return rc;
}
