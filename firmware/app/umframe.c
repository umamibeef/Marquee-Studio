/* UMFRAME codec -- see umframe.h. Freestanding C: no libc, no allocation. */
#include "umframe.h"

uint16_t umf_crc16(const uint8_t *p, int n)
{
	uint16_t c = 0xFFFF;
	for (int i = 0; i < n; i++) {
		c ^= (uint16_t)p[i] << 8;
		for (int b = 0; b < 8; b++)
			c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
	}
	return c;
}

/* COBS: replace each zero byte with the distance to the next zero; a 0xFF code marks a 254-byte run with no zero. */
int umf_cobs_encode(const uint8_t *in, int n, uint8_t *out)
{
	int code_i = 0, o = 1;      /* out[0] reserved for the first code */
	uint8_t code = 1;
	for (int i = 0; i < n; i++) {
		if (in[i]) {
			out[o++] = in[i];
			if (++code == 0xFF) { out[code_i] = code; code_i = o++; code = 1; }
		} else {
			out[code_i] = code; code_i = o++; code = 1;
		}
	}
	out[code_i] = code;
	return o;
}

int umf_cobs_decode(const uint8_t *in, int n, uint8_t *out)
{
	int i = 0, o = 0;
	while (i < n) {
		uint8_t code = in[i++];
		if (code == 0) return -1;                 /* an in-band zero: malformed */
		for (int j = 1; j < code; j++) {
			if (i >= n) return -1;                /* code claims more bytes than present */
			out[o++] = in[i++];
		}
		if (code < 0xFF && i < n) out[o++] = 0;   /* block ended on a zero (not the final block) */
	}
	return o;
}

int umf_build(uint8_t dest, uint8_t src, uint8_t type, uint16_t seq,
              const uint8_t *payload, int len, uint8_t *wire, int cap)
{
	if (len < 0 || len > UMF_MAXPAYLOAD) return -1;
	uint8_t body[UMF_DECMAX];
	body[0] = dest; body[1] = src; body[2] = type;
	body[3] = (uint8_t)(seq >> 8); body[4] = (uint8_t)seq;
	body[5] = (uint8_t)(len >> 8); body[6] = (uint8_t)len;
	for (int i = 0; i < len; i++) body[UMF_HDR + i] = payload[i];
	uint16_t crc = umf_crc16(body, UMF_HDR + len);
	body[UMF_HDR + len] = (uint8_t)(crc >> 8);
	body[UMF_HDR + len + 1] = (uint8_t)crc;
	int bn = UMF_HDR + len + 2;
	if (cap < 2 + bn + bn / 254 + 1) return -1;   /* SOF + COBS worst case + terminator */
	wire[0] = UMF_SOF;
	int en = umf_cobs_encode(body, bn, wire + 1);
	wire[1 + en] = 0x00;
	return 2 + en;
}

int umf_parse(const uint8_t *body, int n, struct umf *f, uint8_t *dec, int cap)
{
	int dn = umf_cobs_decode(body, n, dec);
	if (dn < UMF_HDR + 2 || dn > cap) return -1;
	int len = (dec[5] << 8) | dec[6];
	if (len != dn - (UMF_HDR + 2)) return -1;
	uint16_t crc = umf_crc16(dec, UMF_HDR + len);
	if ((uint8_t)(crc >> 8) != dec[UMF_HDR + len] || (uint8_t)crc != dec[UMF_HDR + len + 1]) return -1;
	f->dest = dec[0]; f->src = dec[1]; f->type = dec[2];
	f->seq = (uint16_t)((dec[3] << 8) | dec[4]);
	f->payload = dec + UMF_HDR; f->len = len;
	return len;
}
