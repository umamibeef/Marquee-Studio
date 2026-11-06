/* Pixels, glyph parts and proportional text: image.ts and font.ts. The base engine: no native emoji or 0x1F commands
 * (native_emoji.c's ext_* layer them on). */
#include "engine.h"
#include "rt.h"

void image_blit__orig(struct image *src, struct image *dst, struct rect *r, struct point *pos)
{
	int32_t sx = r->x, sy = r->y, w = r->w, h = r->h, x = pos->x, y = pos->y, yy;

	if (sx < 0)
		x -= sx, w += sx, sx = 0;
	if (sy < 0)
		y -= sy, h += sy, sy = 0;
	if (w > (int32_t)src->w - sx)
		w = (int32_t)src->w - sx;
	if (h > (int32_t)src->h - sy)
		h = (int32_t)src->h - sy;
	if (x < 0)
		sx -= x, w += x, x = 0;
	if (y < 0)
		sy -= y, h += y, y = 0;
	if (w > (int32_t)dst->w - x)
		w = (int32_t)dst->w - x;
	if (h > (int32_t)dst->h - y)
		h = (int32_t)dst->h - y;
	if (w <= 0 || h <= 0 || !src->pixels || !dst->pixels)
		return;
	for (yy = 0; yy < h; yy++)
		memcpy(dst->pixels + (uint32_t)(y + yy) * dst->w + (uint32_t)x,
		       src->pixels + (uint32_t)(sy + yy) * src->w + (uint32_t)sx, (uint32_t)w * 2u);
}

static void put_clipped(struct image *dst, int32_t x, int32_t y, uint16_t c)
{
	if (x >= 0 && y >= 0 && x < (int32_t)dst->w && y < (int32_t)dst->h)
		dst->pixels[(uint32_t)y * dst->w + (uint32_t)x] = c;
}

void draw_line(struct image *dst, struct point *from, struct point *to, uint32_t colour)
{
	uint16_t c = (uint16_t)colour;
	int64_t ddx = (int64_t)to->x - from->x, ddy = (int64_t)to->y - from->y;

	if (!dst->pixels)
		return;
	if (ddx == 0) {
		int32_t a = from->y < to->y ? from->y : to->y, b = from->y < to->y ? to->y : from->y, x = from->x;

		if (x < 0 || x >= (int32_t)dst->w || b < 0 || a >= (int32_t)dst->h)
			return;
		a = a < 0 ? 0 : a;
		b = b >= (int32_t)dst->h ? (int32_t)dst->h - 1 : b;
		for (; a <= b; a++)
			dst->pixels[(uint32_t)a * dst->w + (uint32_t)x] = c;
		return;
	}
	if (ddx > 0x7FFF || ddx < -0x7FFF || ddy > 0x7FFF || ddy < -0x7FFF)
		return;
	int32_t x = from->x, y = from->y, dx = (int32_t)(ddx < 0 ? -ddx : ddx), dy = (int32_t)(ddy < 0 ? -ddy : ddy);
	int32_t sx = ddx < 0 ? -1 : 1, sy = ddy < 0 ? -1 : 1, err = dx - dy;

	for (;;) {
		put_clipped(dst, x, y, c);
		if (x == to->x && y == to->y)
			return;
		int32_t e2 = 2 * err;
		if (e2 > -dy)
			err -= dy, x += sx;
		if (e2 < dx)
			err += dx, y += sy;
	}
}

/* The part of the cell, clipped to the font image and to dst: font.ts blitMask's numpy-slice clipping. */
static int part_clip(const struct image *dst, const struct font *f, uint32_t ch, const struct rect *part,
                     const struct point *pos, int32_t *sx, int32_t *sy, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2)
{
	int32_t cw = (int32_t)(f->img.w >> 4), chh = (int32_t)font_cell_h(f);
	int32_t mw, mh;

	*sx = (int32_t)(ch & 15) * cw + part->x;
	*sy = (int32_t)((ch & 255) >> 4) * chh + part->y;
	mw = part->w < (int32_t)f->img.w - *sx ? part->w : (int32_t)f->img.w - *sx;
	mh = part->h < (int32_t)f->img.h - *sy ? part->h : (int32_t)f->img.h - *sy;
	mw = mw < 0 ? 0 : mw;
	mh = mh < 0 ? 0 : mh;
	int64_t ex = (int64_t)pos->x + mw, ey = (int64_t)pos->y + mh;
	*x1 = pos->x < 0 ? 0 : pos->x;
	*y1 = pos->y < 0 ? 0 : pos->y;
	*x2 = ex < (int64_t)dst->w ? (int32_t)ex : (int32_t)dst->w;
	*y2 = ey < (int64_t)dst->h ? (int32_t)ey : (int32_t)dst->h;
	return *x2 > *x1 && *y2 > *y1 && dst->pixels && f->img.pixels;
}

void font_draw_char_part(struct image *dst, struct point *pos, struct font *f, uint32_t ch, struct rect *part,
                         uint32_t fg, uint32_t bg)
{
	int32_t sx, sy, x1, y1, x2, y2, xx, yy;

	if (!part_clip(dst, f, ch, part, pos, &sx, &sy, &x1, &y1, &x2, &y2))
		return;
	for (yy = y1; yy < y2; yy++) {
		const uint16_t *s = f->img.pixels + (uint32_t)(sy + yy - pos->y) * f->img.w + sx - pos->x;
		uint16_t *d = dst->pixels + (uint32_t)yy * dst->w;

		for (xx = x1; xx < x2; xx++)
			d[xx] = s[xx] ? (uint16_t)fg : (uint16_t)bg;
	}
}

void font_draw_cell_part(struct image *dst, struct point *pos, struct font *f, uint32_t ch, struct rect *part)
{
	int32_t sx, sy, x1, y1, x2, y2, xx, yy;

	if (!part_clip(dst, f, ch, part, pos, &sx, &sy, &x1, &y1, &x2, &y2))
		return;
	for (yy = y1; yy < y2; yy++) {
		const uint16_t *s = f->img.pixels + (uint32_t)(sy + yy - pos->y) * f->img.w + sx - pos->x;
		uint16_t *d = dst->pixels + (uint32_t)yy * dst->w;

		for (xx = x1; xx < x2; xx++)
			d[xx] = s[xx];
	}
}

int text_width__orig(struct font *f, struct sbuf *t)
{
	int w = 0;
	unsigned prev = 1, i;

	for (i = 0; i < t->len; i++) {
		unsigned c = t->data[i];

		if (f->proportional[c])
			w += (prev == 0 ? 1 : 0) + f->width[c] + 1;
		else
			w += f->width[c];
		prev = f->proportional[c];
	}
	return w;
}

/* The two plain drawers: colours == 0 draws cells in their own colours, else colours[0] / colours[1]. */
static int32_t draw_prop(struct image *dst, struct point *pos, struct font *f, struct sbuf *t, const uint16_t *colours)
{
	int32_t h = (int32_t)font_cell_h(f);
	uint32_t bg = colours ? colours[1] : 0;
	struct point top = *pos, bottom = {pos->x, (int32_t)((uint32_t)pos->y + (uint32_t)h)};
	unsigned prev = 1, i;

	for (i = 0; i < t->len; i++) {
		unsigned c = t->data[i], p = f->proportional[c];
		struct rect part = {f->left[c], 0, f->width[c], h};

		if (!prev && p) {
			draw_line(dst, &top, &bottom, 0);
			top.x++, bottom.x++;
		}
		if (colours)
			font_draw_char_part(dst, &top, f, c, &part, colours[0], colours[1]);
		else
			font_draw_cell_part(dst, &top, f, c, &part);
		top.x += f->width[c], bottom.x += f->width[c];
		if (p) {
			draw_line(dst, &top, &bottom, bg);
			top.x++, bottom.x++;
		}
		prev = p;
	}
	return top.x - pos->x;
}

void text_draw_proportional__orig(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                  uint16_t *colours, uint16_t *width_out)
{
	*width_out = (uint16_t)draw_prop(dst, pos, f, t, colours);
}

void text_draw_proportional_plain__orig(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                        int16_t *width_out)
{
	*width_out = (int16_t)draw_prop(dst, pos, f, t, 0);
}

static void clip_sep(struct image *dst, struct point *at, struct point *fg_origin, struct point *bg_origin,
                     struct font *f, unsigned i, int32_t h, uint32_t eff)
{
	struct rect col = {0, 0, 1, h};

	font_draw_char_effect(dst, at, fg_origin, bg_origin, f, ' ', i, &col, eff);
	at->x++;
}

void text_draw_proportional_clip_maybe__orig(struct image *dst, struct point *pos, struct point *fg_origin,
                                             struct point *bg_origin, uint32_t unused, struct font *f,
                                             struct sbuf *t, const uint32_t *effects, uint32_t n_effects,
                                             uint32_t unused2, int16_t *width_out)
{
	int32_t h = (int32_t)font_cell_h(f);
	uint32_t eff = 0x7FE00000u;
	struct point at = *pos;
	unsigned prev = 1, i;

	(void)unused, (void)unused2;
	for (i = 0; i < t->len; i++) {
		unsigned c = t->data[i], p = f->proportional[c];
		struct rect part = {f->left[c], 0, f->width[c], h};

		if (!prev && p)
			clip_sep(dst, &at, fg_origin, bg_origin, f, i, h, eff);
		eff = i < n_effects ? effects[i] : n_effects ? effects[n_effects - 1] : 0x7FE00000u;
		font_draw_char_effect(dst, &at, fg_origin, bg_origin, f, c, i, &part, eff);
		at.x += f->width[c];
		if (p)
			clip_sep(dst, &at, fg_origin, bg_origin, f, i, h, eff);
		prev = p;
	}
	*width_out = (int16_t)(at.x - pos->x);
}
