/* Autodesk FLIC as the sign plays it: flic.ts. COLOR256/64, BRUN, LC, SS2, BLACK, COPY, and the sign's 16-bit variant
 * (BRUN over 2 * width bytes per row, COPY of little-endian RGB555). Every read and write is bounds-checked: reads
 * past the file give 0 (the TypeScript's undefined acts the same way in these expressions) and the decoders stop
 * there, writes outside the image are dropped. */
#include "engine.h"
#include "rt.h"

static inline uint32_t u8_(const struct flic *fl, uint32_t o) { return o < fl->size ? fl->d[o] : 0; }
static inline uint32_t u16_(const struct flic *fl, uint32_t o) { return u8_(fl, o) | u8_(fl, o + 1) << 8; }
static inline uint32_t u32_(const struct flic *fl, uint32_t o) { return u16_(fl, o) | u16_(fl, o + 2) << 16; }
static inline int32_t i8_(uint32_t v) { return (int32_t)(int8_t)(uint8_t)v; }

static inline void setpx(struct flic *fl, uint32_t i, uint32_t v)
{
	if (i < fl->w * fl->h)
		fl->px[i] = (uint16_t)v;
}

void flic_close(struct flic *fl)
{
	free(fl->off);
	free(fl->px);
	free(fl->img);
	fl->off = 0, fl->px = 0, fl->img = 0;
}

int flic_open(struct flic *fl, const uint8_t *data, uint32_t size)
{
	memset(fl, 0, sizeof *fl);
	fl->d = data, fl->size = size;
	if (size < 16)
		return 0;
	uint32_t magic = u16_(fl, 4);
	if (magic != 0xAF11 && magic != 0xAF12)
		return 0;
	fl->frames = u16_(fl, 6), fl->w = u16_(fl, 8), fl->h = u16_(fl, 10), fl->depth = u16_(fl, 12);
	uint32_t speed = u32_(fl, 0x10);
	if (magic == 0xAF11) {
		/* speed * 100 / 7 without 64-bit division: anything this large is the slowest hold anyway */
		speed = speed > 100000u ? 0xFFFFFFFFu : speed * 100u / 7u;
	}
	fl->speed_ms = speed;
	uint32_t n = fl->w * fl->h;
	/* frameOffsets */
	uint32_t oframe1 = size >= 0x54 ? u32_(fl, 0x50) : 0;
	uint32_t start = oframe1 >= 128 && oframe1 < size ? oframe1 : 128, off, count = 0;
	for (off = start; off + 16 <= size;) {
		uint32_t fsize = u32_(fl, off);
		if (u16_(fl, off + 4) == 0xF1FA)
			count++;
		if (fsize > size - off)
			break;
		off += fsize ? fsize : 16;
	}
	fl->off = malloc((count ? count : 1) * sizeof(uint32_t));
	fl->px = malloc((n ? n : 1) * 2);
	fl->img = malloc((n ? n : 1) * 2);
	if (!fl->off || !fl->px || !fl->img) {
		flic_close(fl);
		return 0;
	}
	memset(fl->px, 0, n * 2);
	memset(fl->img, 0, n * 2);
	for (off = start; off + 16 <= size;) {
		uint32_t fsize = u32_(fl, off);
		if (u16_(fl, off + 4) == 0xF1FA)
			fl->off[fl->noff++] = off;
		if (fsize > size - off)
			break;
		off += fsize ? fsize : 16;
	}
	return 1;
}

static void palette8(struct flic *fl, uint32_t off, int div64)
{
	uint32_t npk = u16_(fl, off), idx = 0, k, j;

	off += 2;
	for (k = 0; k < npk && off < fl->size; k++) {
		uint32_t skip = u8_(fl, off), cnt = u8_(fl, off + 1), n;
		off += 2;
		idx += skip;
		n = cnt ? cnt : 256;
		for (j = 0; j < n; j++) {
			uint32_t r = u8_(fl, off), g = u8_(fl, off + 1), b = u8_(fl, off + 2);
			off += 3;
			if (div64)
				r <<= 2, g <<= 2, b <<= 2;
			if (idx < 256)
				fl->palette[idx * 3] = (uint16_t)r, fl->palette[idx * 3 + 1] = (uint16_t)g, fl->palette[idx * 3 + 2] = (uint16_t)b;
			idx++;
		}
	}
}

static void brun(struct flic *fl, uint32_t off)
{
	uint32_t W = fl->w, y;

	for (y = 0; y < fl->h; y++) {
		uint32_t x = 0;
		off += 1;
		while (x < W && off < fl->size) {
			uint32_t c = u8_(fl, off++), i;
			if (c & 0x80) {
				uint32_t n = 256 - c;
				for (i = 0; i < n; i++, x++)
					if (x < W)
						fl->px[y * W + x] = (uint16_t)u8_(fl, off + i);
				off += n;
			} else {
				uint32_t v = u8_(fl, off);
				for (i = 0; i < c; i++, x++)
					if (x < W)
						fl->px[y * W + x] = (uint16_t)v;
				off += 1;
			}
		}
	}
}

static void put_byte16(struct flic *fl, uint32_t y, uint32_t b, uint32_t v)
{
	uint32_t W = fl->w, i = y * W + b / 2;

	if (b >= 2 * W)
		return;
	if (b & 1)
		fl->px[i] = (uint16_t)(fl->px[i] | v << 8);
	else
		fl->px[i] = (uint16_t)v;
}

static void brun16(struct flic *fl, uint32_t off)
{
	uint32_t W2 = 2 * fl->w, y;

	for (y = 0; y < fl->h; y++) {
		uint32_t x = 0;
		off += 1;
		while (x < W2 && off < fl->size) {
			uint32_t c = u8_(fl, off++), i;
			if (c & 0x80) {
				uint32_t n = 256 - c;
				for (i = 0; i < n; i++, x++)
					put_byte16(fl, y, x, u8_(fl, off + i));
				off += n;
			} else {
				uint32_t v = u8_(fl, off);
				for (i = 0; i < c; i++, x++)
					put_byte16(fl, y, x, v);
				off += 1;
			}
		}
	}
}

static void lc(struct flic *fl, uint32_t off)
{
	uint32_t W = fl->w, y = u16_(fl, off), ny = u16_(fl, off + 2), k, j;

	off += 4;
	for (k = 0; k < ny && off < fl->size; k++, y++) {
		uint32_t npk = u8_(fl, off++), x = 0;
		for (j = 0; j < npk; j++) {
			uint32_t skip = u8_(fl, off), i;
			int32_t sz = i8_(u8_(fl, off + 1));
			off += 2;
			x += skip;
			if (sz >= 0) {
				for (i = 0; i < (uint32_t)sz; i++)
					setpx(fl, y * W + x + i, u8_(fl, off + i));
				off += (uint32_t)sz;
				x += (uint32_t)sz;
			} else {
				uint32_t n = (uint32_t)-sz, c = u8_(fl, off++);
				for (i = 0; i < n; i++)
					setpx(fl, y * W + x + i, c);
				x += n;
			}
		}
	}
}

static void ss2(struct flic *fl, uint32_t off)
{
	uint32_t W = fl->w, ny = u16_(fl, off), y = 0, done = 0;

	off += 2;
	while (done < ny && off < fl->size) {
		uint32_t word = u16_(fl, off), j, x = 0;

		off += 2;
		if (word & 0x8000) {
			if (word & 0x4000) {
				y += 0x10000 - word;
				continue;
			}
			setpx(fl, y * W + (W - 1), word & 0xFF);
			done++, y++;
			continue;
		}
		for (j = 0; j < word; j++) {
			uint32_t skip = u8_(fl, off), i;
			int32_t sz = i8_(u8_(fl, off + 1));
			off += 2;
			x += skip;
			if (sz >= 0) {
				for (i = 0; i < (uint32_t)sz; i++) {
					setpx(fl, y * W + x + i * 2, u8_(fl, off + i * 2));
					setpx(fl, y * W + x + i * 2 + 1, u8_(fl, off + i * 2 + 1));
				}
				off += (uint32_t)sz * 2;
				x += (uint32_t)sz * 2;
			} else {
				uint32_t n = (uint32_t)-sz, a = u8_(fl, off), b = u8_(fl, off + 1);
				off += 2;
				for (i = 0; i < n; i++) {
					setpx(fl, y * W + x + i * 2, a);
					setpx(fl, y * W + x + i * 2 + 1, b);
				}
				x += n * 2;
			}
		}
		done++, y++;
	}
}

void flic_decode(struct flic *fl, uint32_t off)
{
	uint32_t fsize = u32_(fl, off), nsub = u16_(fl, off + 6), p = off + 16, k, i, n = fl->w * fl->h;

	for (k = 0; k < nsub; k++) {
		if (p + 6 > off + fsize)
			break;
		uint32_t csz = u32_(fl, p), ctyp = u16_(fl, p + 4), body = p + 6;

		if (fl->depth == 16) {
			if (ctyp == 15)
				brun16(fl, body);
			else if (ctyp == 16)
				for (i = 0; i < n; i++)
					fl->px[i] = (uint16_t)u16_(fl, body + 2 * i);
			else if (ctyp == 13)
				memset(fl->px, 0, n * 2);
		} else if (ctyp == 4 || ctyp == 11)
			palette8(fl, body, ctyp == 11);
		else if (ctyp == 15)
			brun(fl, body);
		else if (ctyp == 12)
			lc(fl, body);
		else if (ctyp == 7)
			ss2(fl, body);
		else if (ctyp == 13)
			memset(fl->px, 0, n * 2);
		else if (ctyp == 16)
			for (i = 0; i < n; i++)
				fl->px[i] = (uint16_t)u8_(fl, body + i);
		p += csz ? csz : 6;
	}
	/* image555 */
	if (fl->depth == 16) {
		memcpy(fl->img, fl->px, n * 2);
		return;
	}
	static uint16_t pal[256];
	for (i = 0; i < 256; i++)
		pal[i] = (uint16_t)(((fl->palette[i * 3] >> 3) & 31u) << 10 | ((fl->palette[i * 3 + 1] >> 3) & 31u) << 5 |
		                    ((fl->palette[i * 3 + 2] >> 3) & 31u));
	for (i = 0; i < n; i++)
		fl->img[i] = pal[fl->px[i] & 0xFF];
}
