/* Colour emoji for ELITE_RGB V4.20: draw font cells 0x80..0xFF of an alpha (format 9) font in their own
 * colours. Reached from hook.S, which detours font_draw_char (0x42DB0); all other characters, and all
 * fonts without alpha, keep the firmware's own mask drawing.
 *
 * Firmware facts used here (see analysis/symbols.tsv):
 *   image  { u32 format, w, h; u16 *pixels; u16 *palette; }   format 1 RGB555, 9 RGB555 + alpha
 *   font   { u32 type; image img; ... }                          type 2 = 16x16 grid, 1 = 16x8, 0 = digits
 *   format-9 pixel: bit15 clear = opaque RGB555;
 *                   bit15 set   = translucent: alpha3 in bits 12..14, RGB444 in bits 11..0
 *   font_glyph_locate(font, ch, &col, &row) at 0x444C8 returns nonzero if the character exists. */
#include <stdint.h>

struct image {
	uint32_t format, w, h;
	uint16_t *pixels;
	uint16_t *palette;
};

struct font {
	uint32_t type;
	struct image img;
};

#define font_glyph_locate ((int (*)(struct font *, unsigned, int *, int *))0x444C8)

static unsigned cell_height(const struct font *f)
{
	if (f->type == 1)
		return f->img.h >> 3;
	if (f->type == 0)
		return f->img.h;
	return f->img.h >> 4;
}

/* Same arguments as font_draw_char: (frame, pos {x, y}, font, character, fg, bg). fg/bg are unused:
 * a colour glyph keeps its own colours. */
void colour_draw_char(struct image *dst, const int *pos, struct font *f, unsigned ch,
                      unsigned fg, unsigned bg)
{
	int col, row;
	unsigned cw, chh, x, y;
	const uint16_t *src;

	(void)fg;
	(void)bg;
	if (!font_glyph_locate(f, ch & 0xFF, &col, &row))
		return;
	cw = f->img.w >> 4;
	chh = cell_height(f);
	src = f->img.pixels + (uint32_t)row * chh * f->img.w + (uint32_t)col * cw;

	for (y = 0; y < chh; y++) {
		int dy = pos[1] + (int)y;
		uint16_t *drow;

		if (dy < 0 || dy >= (int)dst->h)
			continue;
		drow = dst->pixels + (uint32_t)dy * dst->w;
		for (x = 0; x < cw; x++) {
			int dx = pos[0] + (int)x;
			unsigned p = src[y * f->img.w + x];
			unsigned a, d, r, g, b;

			if (dx < 0 || dx >= (int)dst->w)
				continue;
			if (!(p & 0x8000)) {                     /* opaque */
				drow[dx] = (uint16_t)p;
				continue;
			}
			a = (p >> 12) & 7;
			if (a == 0)                              /* transparent */
				continue;
			a = a * 4 + 3;                           /* 7..31 out of 31 */
			d = drow[dx];
			r = (a * (((p >> 8) & 0xF) << 1 | 1) + (31 - a) * ((d >> 10) & 0x1F)) / 31;
			g = (a * (((p >> 4) & 0xF) << 1 | 1) + (31 - a) * ((d >> 5) & 0x1F)) / 31;
			b = (a * ((p & 0xF) << 1 | 1) + (31 - a) * (d & 0x1F)) / 31;
			drow[dx] = (uint16_t)(r << 10 | g << 5 | b);
		}
	}
}
