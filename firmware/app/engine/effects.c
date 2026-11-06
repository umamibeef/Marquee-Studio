/* Effect words FG<<16 | BG, slots, the mask blit and the outline: effects.ts. No textures are built: a layer is
 * resolved to a small description and each pixel computed from it. */
#include "engine.h"
#include "rt.h"
#include "../effect_slots.h"

#define TRANSPARENT 0xFFFFu
#define YELLOW 0x7FE0u
#define NSLOTS 100

uint32_t effect_frame_count;

static const uint16_t CYCLE[3] = {0x7C00, 0x03E0, 0x7FE0};   /* red, green, yellow */

/* ---- slots ---- */
enum { SRC_UNSET, SRC_NONE, SRC_BUILTIN, SRC_FLC };
#define NGLITTER 8
static uint16_t glitter_buf[NGLITTER][1280];
static uint8_t glitter_next;

struct slot {
	uint8_t src, failed, gl;              /* gl: 1 + index in glitter_buf */
	uint32_t stamp, last, hold, count, index, n;
	uint16_t tw, th;
	const uint16_t *tex;                  /* 0: an all-zero tile (nothing decoded yet) */
	const struct eslot *e;
	struct flic *fl;
	uint16_t solid;                       /* failed: the 1 x 1 yellow, or the failed FLC's fill */
};
static struct slot slots[NSLOTS];
static effect_flc_provider provider;

uint32_t flc_speed_to_hold(uint32_t speed)
{
	uint32_t i = 1;

	if (speed <= 38)
		return 0;
	while (i < 40 && 38 + 25 * i < speed)
		i++;
	return i;
}

static void slot_clear(struct slot *s)
{
	if (s->fl) {
		flic_close(s->fl);
		free(s->fl);
	}
	memset(s, 0, sizeof *s);
}

void effects_reset(uint32_t fc)
{
	unsigned n;

	for (n = 0; n < NSLOTS; n++)
		slot_clear(&slots[n]);
	glitter_next = 0;
	provider = 0;
	effect_frame_count = fc;
}

void effect_slot_set_provider(effect_flc_provider p) { provider = p; }

static int slot_use_flc(struct slot *s, const uint8_t *data, uint32_t size)
{
	struct flic *fl = malloc(sizeof *fl);

	if (!fl)
		return 0;
	if (!flic_open(fl, data, size)) {
		free(fl);
		return 0;
	}
	slot_clear(s);
	s->src = SRC_FLC;
	s->fl = fl;
	s->n = fl->frames;
	s->hold = flc_speed_to_hold(fl->speed_ms);
	s->tw = (uint16_t)fl->w, s->th = (uint16_t)fl->h;
	s->tex = fl->img;
	return 1;
}

static void slot_use_builtin(struct slot *s, unsigned n)
{
	slot_clear(s);
	if (n >= NSLOT) {
		s->src = SRC_NONE, s->failed = 1, s->solid = YELLOW;
		return;
	}
	const struct eslot *e = &eslots[n];
	s->src = SRC_BUILTIN;
	s->e = e;
	s->n = e->nframes;
	s->hold = e->hold ? e->hold - 1u : 0;     /* eslot.hold: panel frames per frame; flc_tick shows hold + 1 */
	s->tw = e->w, s->th = e->h;
}

int effect_slot_set_flc(unsigned n, const uint8_t *data, uint32_t size)
{
	if (n >= NSLOTS)
		return 0;
	if (!data) {
		slot_use_builtin(&slots[n], n);
		return 1;
	}
	return slot_use_flc(&slots[n], data, size);
}

static struct slot *slot_get(unsigned n)
{
	struct slot *s = &slots[n];

	if (s->src == SRC_UNSET) {
		const uint8_t *data;
		uint32_t size;

		if (!(provider && provider(n, &data, &size) && slot_use_flc(s, data, size)))
			slot_use_builtin(s, n);
	}
	return s;
}

uint32_t effect_slot_period(unsigned n)
{
	if (n >= NSLOTS)
		return 1;
	const struct slot *s = slot_get(n);
	if (s->src == SRC_NONE)
		return 1;
	return (s->n ? s->n : 1) * (s->hold + 1);
}

static void slot_show_builtin(struct slot *s)
{
	const struct eslot *e = s->e;

	if (e->px) {
		s->tex = e->px + s->index * (uint32_t)e->w * e->h;
		return;
	}
	uint32_t n = (uint32_t)e->w * e->h, i;                 /* glitter: each pixel walks its ramp, with its own phase */
	if (n > 1280 || (!s->gl && glitter_next >= NGLITTER)) {
		s->failed = 1, s->solid = YELLOW;
		return;
	}
	if (!s->gl)
		s->gl = (uint8_t)++glitter_next;
	uint16_t *b = glitter_buf[s->gl - 1];
	for (i = 0; i < n; i++) {
		uint32_t p = (s->index + glitter_phase[i]) % e->nframes;
		b[i] = e->ramp[p < e->levels ? p : e->nframes - p];
	}
	s->tex = b;
}

static void slot_rewind(struct slot *s)
{
	if (!s->failed)
		s->index = 0;
}

/* flc_tick: 1 playing, 2 decoded the last frame, 0 failed */
static int slot_tick(struct slot *s, uint32_t fc)
{
	if (s->failed)
		return 0;
	if (s->last + 8u < fc)
		slot_rewind(s);
	s->last = fc;
	if (s->count) {
		s->count--;
		return 1;
	}
	s->count = s->hold;
	if (s->src == SRC_FLC) {
		struct flic *fl = s->fl;

		if (s->index >= fl->noff) {
			uint32_t i, m = fl->w * fl->h;

			s->failed = 1;
			for (i = 0; i < m; i++)
				fl->img[i] = fl->depth != 16 ? YELLOW : 0;
			return 0;
		}
		flic_decode(fl, fl->off[s->index]);
	} else {
		slot_show_builtin(s);
		if (s->failed)
			return 0;
	}
	if (s->index + 1 < s->n) {
		s->index++;
		return 1;
	}
	return 2;
}

/* ---- layers ---- */
enum { L_SOLID, L_TEX, L_WIPE, L_STRIPES };
struct layer {
	uint8_t kind, anchored;
	uint16_t colour, a, b;                /* L_SOLID: colour. L_WIPE: a above the moving row, b below it */
	uint32_t tw, th, t;                   /* tile size; L_WIPE: t the moving row; L_STRIPES: t the time */
	const uint16_t *d;                    /* L_TEX */
};

/* effect_resolve */
static int resolve(uint32_t code, uint32_t phase, int32_t cell_h, struct layer *l)
{
	uint32_t fc = effect_frame_count;

	l->kind = L_SOLID, l->anchored = 1, l->colour = 0, l->d = 0, l->tw = l->th = 1;
	if (code < 0x8000u) {
		l->colour = (uint16_t)code;
		return 1;
	}
	if ((code & 0xC000u) == 0x8000u) {
		if (code == 0x8002u) {                /* diagonal stripes, a 12 x 12 tile moving 1 px per 2 frames */
			l->kind = L_STRIPES, l->tw = l->th = 12, l->t = fc >> 1;
			return 1;
		}
		if (code == 0xBFFEu || code == 0xBFFFu) {
			uint32_t v = (uint32_t)((int32_t)fc >> 4) + (code == 0xBFFEu ? phase : 0u - phase);
			l->colour = CYCLE[v % 3];
			return 1;
		}
		if (code == 0x8000u) {
			l->colour = TRANSPARENT;
			return 1;
		}
		if (code == 0x8001u) {                /* a column cell_h high, wiping down a row per 4 frames */
			static const uint16_t AB[3][2] = {{0x7C00, 0x03E0}, {0x7FE0, 0x7C00}, {0x03E0, 0x7FE0}};
			uint32_t t = fc >> 2, h;
			if (cell_h <= 0)                  /* (effects.ts throws here: nothing can be drawn at this height) */
				return 0;
			h = (uint32_t)cell_h;
			l->kind = L_WIPE, l->anchored = 0, l->tw = 1, l->th = h, l->t = t % h;
			l->a = AB[(t / h) % 3][0], l->b = AB[(t / h) % 3][1];
			return 1;
		}
		l->colour = YELLOW;                   /* 0x8003..0xBFFD: nothing is drawn */
		return 0;
	}
	uint32_t n = code & 0x3FFFu;
	if (n >= NSLOTS) {
		l->colour = YELLOW;
		return 1;
	}
	struct slot *s = slot_get(n);
	if (s->stamp != fc) {
		s->stamp = fc;
		if (slot_tick(s, fc) != 1)
			slot_rewind(s);
	}
	if (s->failed && s->src != SRC_FLC) {     /* the 1 x 1 yellow */
		l->kind = L_TEX, l->d = &s->solid, l->tw = l->th = 1;
		return 1;
	}
	l->kind = L_TEX, l->anchored = 0, l->d = s->tex, l->tw = s->tw, l->th = s->th;   /* anchored 0: the slot fill moves WITH the text, not fixed to the panel (diverges from stock) */
	return 1;
}

/* glyph_desc_from_effect: 0 when nothing is drawn; the BG half is not resolved when the FG half fails */
static int desc_of(uint32_t word, uint32_t phase, int32_t cell_h, struct layer *fg, struct layer *bg)
{
	return resolve(word >> 16, phase, cell_h, fg) && resolve(word & 0xFFFFu, phase, cell_h, bg);
}

/* rect_clip: clip r to bounds in place; edges that only touch still count */
static int rect_clip(int32_t bx, int32_t by, int32_t bw, int32_t bh, struct rect *r)
{
	int64_t bxe = (int64_t)bx + bw, bye = (int64_t)by + bh, rxe = (int64_t)r->x + r->w, rye = (int64_t)r->y + r->h;

	if (!(r->y <= bye && by <= rye && r->x <= bxe && bx <= rxe))
		return 0;
	if (r->x < bx) {
		r->w = (int32_t)(rxe - bx);
		r->x = bx;
	}
	if (bxe < (int64_t)r->x + r->w)
		r->w = (int32_t)(bxe - r->x);
	if (r->y < by) {
		r->h = (int32_t)(rye - by);
		r->y = by;
	}
	if (bye < (int64_t)r->y + r->h)
		r->h = (int32_t)(bye - r->y);
	return 1;
}

static inline uint16_t layer_px(const struct layer *l, uint32_t col, uint32_t row)
{
	switch (l->kind) {
	case L_TEX:
		return l->d ? l->d[row * l->tw + col] : 0;
	case L_WIPE:
		return row > l->t ? l->b : l->a;
	case L_STRIPES:
		return CYCLE[((col + row - l->t) % 12u) / 4u];
	default:
		return l->colour;
	}
}

/* glyph_blit mode 8: src as a mask (pixel != 0); r the source rect, clipped in place; pos the destination */
static void mask_blit(struct image *src, struct image *dst, const struct layer *fg, const struct layer *bg, struct rect *r,
                      const struct point *pos, const struct point *fg_origin, const struct point *bg_origin)
{
	if (!src->pixels || !dst->pixels)
		return;
	if (!rect_clip(0, 0, (int32_t)src->w, (int32_t)src->h, r))
		return;
	struct rect d = {pos->x, pos->y, r->w, r->h};
	if (!rect_clip(0, 0, (int32_t)dst->w, (int32_t)dst->h, &d))
		return;
	r->x -= pos->x - d.x;
	r->y -= pos->y - d.y;
	if (d.w <= 0 || d.h <= 0)
		return;
	uint32_t fox = fg->anchored ? (uint32_t)(d.x - pos->x + fg_origin->x) : 0;
	uint32_t foy = fg->anchored ? (uint32_t)(d.y - pos->y + fg_origin->y) : 0;
	uint32_t box = bg->anchored ? (uint32_t)(d.x - pos->x + bg_origin->x) : 0;
	uint32_t boy = bg->anchored ? (uint32_t)(d.y - pos->y + bg_origin->y) : 0;
	/* the mask block is cut short at src's edge, the output block at dst's */
	int32_t mh = (int32_t)src->h - r->y, mw = (int32_t)src->w - r->x;
	mh = mh < 0 ? 0 : mh < d.h ? mh : d.h;
	mw = mw < 0 ? 0 : mw < d.w ? mw : d.w;
	int fg_flat = fg->kind == L_SOLID, bg_flat = bg->kind == L_SOLID;
	uint32_t fcol0 = fg->tw ? fox % fg->tw : 0, frow0 = fg->th ? foy % fg->th : 0;
	uint32_t bcol0 = bg->tw ? box % bg->tw : 0, brow0 = bg->th ? boy % bg->th : 0;
	uint32_t fg_empty = !fg_flat && (!fg->tw || !fg->th), bg_empty = !bg_flat && (!bg->tw || !bg->th);
	uint16_t behind = bg_flat ? bg->colour : 0;
	int32_t u, v;

	for (v = 0; v < d.h; v++) {
		const uint16_t *m = v < mh ? src->pixels + (uint32_t)(r->y + v) * src->w + (uint32_t)r->x : 0;
		uint16_t *o = dst->pixels + (uint32_t)(d.y + v) * dst->w + (uint32_t)d.x;
		uint32_t frow = fg->th ? (frow0 + (uint32_t)v) % fg->th : 0, brow = bg->th ? (brow0 + (uint32_t)v) % bg->th : 0;
		uint32_t fcol = fcol0, bcol = bcol0;

		for (u = 0; u < d.w; u++) {
			int on = m && u < mw && m[u] != 0;

			if (!fg_flat) {
				uint16_t t = fg_empty ? 0 : layer_px(fg, fcol, frow);
				if (bg_flat && bg->colour == TRANSPARENT) {
					if (on)
						o[u] = t;
				} else
					o[u] = on ? t : 0;
			} else if (behind == TRANSPARENT) {
				if (on)
					o[u] = fg->colour;
			} else
				o[u] = on ? fg->colour : behind;
			if (!on) {
				if (!bg_flat)
					o[u] = bg_empty ? 0 : layer_px(bg, bcol, brow);
				else if (!fg_flat && bg->colour != TRANSPARENT)
					o[u] = bg->colour;
			}
			if (++fcol == fg->tw)
				fcol = 0;
			if (++bcol == bg->tw)
				bcol = 0;
		}
	}
}

void glyph_blit_effect__orig(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos)
{
	struct layer fg, bg;

	if (desc_of(effect, 0, eng_s16(r->h), &fg, &bg))
		mask_blit(src, dst, &fg, &bg, r, pos, pos, pos);
}

/* The same, with the fill's texture taken from (col, row) at the rect's first pixel: a moving letter carries its fill. */
void glyph_blit_effect_tex(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos, int32_t col, int32_t row)
{
	struct layer fg, bg;

	if (!desc_of(effect, 0, eng_s16(r->h), &fg, &bg))
		return;
	if (fg.tw)
		col = (col % (int32_t)fg.tw + (int32_t)fg.tw) % (int32_t)fg.tw;
	if (fg.th)
		row = (row % (int32_t)fg.th + (int32_t)fg.th) % (int32_t)fg.th;
	struct point o = {col, row};
	fg.anchored = 1;
	mask_blit(src, dst, &fg, &bg, r, pos, &o, pos);
}

void font_draw_char_effect(struct image *dst, struct point *pos, struct point *fg_origin, struct point *bg_origin,
                           struct font *f, uint32_t ch, uint32_t phase, struct rect *part, uint32_t effect)
{
	struct layer fg, bg;
	struct rect r = {(int32_t)(ch & 15) * (int32_t)(f->img.w >> 4) + part->x,
	                 (int32_t)((ch & 255) >> 4) * (int32_t)font_cell_h(f) + part->y, part->w, part->h};

	if (desc_of(effect, phase & 0xFFFFu, (int32_t)font_cell_h(f), &fg, &bg))
		mask_blit(&f->img, dst, &fg, &bg, &r, pos, fg_origin, bg_origin);
}

/* ---- the outline ---- */
uint32_t effect_colour_index_to_rgb555(int32_t i)
{
	static const uint16_t T[8] = {0x0000, 0x7C00, 0x03E0, 0x7FE0, 0x001F, 0x03FF, 0x7C1F, 0x7FFF};
	return i >= 0 && i < 8 ? T[i] : 0;
}

struct ol {
	uint16_t *d;
	int64_t dn;
	int32_t dw, ox0, oy0;
	const uint16_t *s;
	int64_t sn;
	int32_t sw;
	uint16_t c;
	int32_t px, py;
};

/* The outline goes behind the glyph: the original only writes where the source mask is background, so an outline drawn
 * after the letters rings them instead of flooding them (glyph_outline_blit 0x3ff0c: the writes are gated on the mask). */
static inline void ol_put(const struct ol *o, int32_t x, int32_t y)
{
	int64_t i = (int64_t)y * o->dw + x;

	if (i < 0 || i >= o->dn)
		return;
	int64_t si = (int64_t)(o->oy0 + y - o->py) * o->sw + o->ox0 + x - o->px;
	if (si >= 0 && si < o->sn && o->s[si] != 0)
		return;
	o->d[i] = o->c;
}

static inline int ol_ink(const struct ol *o, int32_t u, int32_t v)
{
	int64_t i = (int64_t)(o->oy0 + v) * o->sw + o->ox0 + u;

	return i >= 0 && i < o->sn && o->s[i] != 0;
}

void glyph_outline_blit(struct image *src, struct rect *sr, struct image *dst, struct point *pos, int32_t *outline,
                        struct rect *clip)
{
	int32_t style = outline[0];
	struct ol o = {dst->pixels, (int64_t)dst->w * dst->h, (int32_t)dst->w, sr->x, sr->y, src->pixels,
	               (int64_t)src->w * src->h, (int32_t)src->w, (uint16_t)effect_colour_index_to_rgb555(outline[1] >> 16),
	               pos->x, pos->y};
	int32_t px = pos->x, py = pos->y, xr = clip->x + clip->w - 1, yb = clip->y + clip->h - 1, cy = clip->y;
	int32_t v, u, last = sr->w - 1;

	if (!o.d || !o.s)
		return;
	for (v = 0; v < sr->h; v++) {
		int32_t Y = v + py;
		int up = cy <= Y - 1 && Y - 1 <= yb, mid = cy <= Y && Y <= yb, dn = cy <= Y + 1 && Y + 1 <= yb;

		if (style == 1 && 0 <= Y && Y <= yb) {
			int inside = 0;
			for (u = 0; u < sr->w; u++) {
				int32_t X = u + px;
				int notright = X < xr, atleft = X <= clip->x;
				if (inside) {
					if (!ol_ink(&o, u, v)) {
						inside = 0;
						if (notright) {
							if (up) ol_put(&o, X, Y - 1);
							if (mid) ol_put(&o, X, Y);
							if (dn) ol_put(&o, X, Y + 1);
						}
					} else {
						if (up) ol_put(&o, X, Y - 1);
						if (dn) ol_put(&o, X, Y + 1);
					}
				} else if (ol_ink(&o, u, v)) {
					inside = 1;
					if (up) {
						ol_put(&o, X, Y - 1);
						if (!atleft) ol_put(&o, X - 1, Y - 1);
					}
					if (mid && !atleft) ol_put(&o, X - 1, Y);
					if (dn) {
						if (!atleft) ol_put(&o, X - 1, Y + 1);
						ol_put(&o, X, Y + 1);
					}
				}
				if (u == last && inside && notright) {
					if (up) ol_put(&o, X + 1, Y - 1);
					if (mid) ol_put(&o, X + 1, Y);
					if (dn) ol_put(&o, X + 1, Y + 1);
				}
			}
		} else if (style == 2 && 0 <= Y && Y <= yb) {
			int inside = 0;
			for (u = 0; u < sr->w; u++) {
				int32_t X = u + px;
				int atright = xr <= X;
				if (inside) {
					if (!ol_ink(&o, u, v)) {
						inside = 0;
						if (mid && !atright) ol_put(&o, X, Y);
					} else {
						if (up) ol_put(&o, X, Y - 1);
						if (dn) ol_put(&o, X, Y + 1);
					}
				} else if (ol_ink(&o, u, v)) {
					inside = 1;
					if (up) ol_put(&o, X, Y - 1);
					if (mid && !(X <= clip->x)) ol_put(&o, X - 1, Y);
					if (dn) ol_put(&o, X, Y + 1);
				}
				if (u == last && inside && mid && !atright) ol_put(&o, X + 1, Y);
			}
		} else if (style == 3 && Y <= yb) {
			int state = 0;
			for (u = 0; u < sr->w; u++) {
				int32_t X = u + px;
				int atright = xr <= X;
				if (state == 0)
					state = ol_ink(&o, u, v);
				else {
					state = ol_ink(&o, u, v);
					if (dn && !atright) ol_put(&o, X, Y + 1);
				}
				if (u == last && state && dn && !atright) ol_put(&o, X + 1, Y + 1);
			}
		}
	}
}
