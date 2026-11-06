/* font_build, the metrics, the automatic picks, MQF1: font.ts and mqf.ts. */
#include "engine.h"
#include "rt.h"

struct font_entry font_table[100];
uint8_t font_count;
struct fonts_state fonts_state;

_Static_assert(sizeof(struct rect) == 16 && sizeof(struct point) == 8, "layout shared with firmware/ext");
#if __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(struct image) == 20, "layout shared with firmware/ext");
_Static_assert(sizeof(struct font) == 0x618, "update.c and bootscreen.c use this size");
_Static_assert(sizeof(struct font_entry) == 0x718, "fonts.c");
_Static_assert(sizeof(struct sbuf) == 8, "layout shared with firmware/ext");
#endif

uint32_t font_cell_h(const struct font *f)
{
	if (f->layout == 1)
		return f->img.h >> 3;
	if (f->layout == 0)
		return f->img.h;
	return f->img.h >> 4;
}

static void measure(struct font *f, uint32_t c)
{
	uint32_t cw = f->img.w >> 4, ch = font_cell_h(f);

	f->proportional[c] = 1;
	if (c == 0x20) {
		uint32_t w = (f->img.w >> 6) & 0xFF;
		f->left[c] = 0;
		f->width[c] = (uint8_t)(w > 3 ? w : 3);
		f->proportional[c] = 0;
		return;
	}
	if (c == 0xFF || (c >= 0x30 && c <= 0x39) || (c >= 0x10 && c <= 0x19)) {
		f->left[c] = 0, f->width[c] = (uint8_t)cw, f->right[c] = 0, f->proportional[c] = 0;
		return;
	}
	const uint16_t *cell = f->img.pixels + ((c >> 4) * ch) * f->img.w + (c & 15) * cw;
	uint32_t left = cw, right = 0, x, y;

	for (x = 0; x < cw && left == cw; x++)
		for (y = 0; y < ch; y++)
			if (cell[y * f->img.w + x]) {
				left = x;
				break;
			}
	if (left < cw)
		for (x = cw; x-- > 0;) {
			for (y = 0; y < ch; y++)
				if (cell[y * f->img.w + x])
					break;
			if (y < ch)
				break;
			right++;
		}
	if (left + right < cw) {
		f->left[c] = (uint8_t)left, f->width[c] = (uint8_t)(cw - (left + right)), f->right[c] = (uint8_t)right;
	} else {
		f->left[c] = (uint8_t)cw, f->width[c] = 0, f->right[c] = 0;
	}
}

/* Layout 2 is font.ts. Layouts 1 and 0 have no TypeScript: they use the same grid with their own cell height
 * (native_emoji.c cell_height); codes past the image's rows measure as empty. Nothing tests them. */
void font_build(struct font *f, uint32_t layout, struct image *img)
{
	struct image in = *img;                 /* (img may be &f->img) */
	uint32_t c, rows;

	f->layout = layout;
	f->img = in;
	memset(f->left, 0, sizeof f->left * 6);
	rows = layout == 2 ? 16 : layout == 1 ? 8 : 1;
	for (c = 0; c < 256; c++)
		if (layout == 2 || (c >> 4) < rows)
			measure(f, c);
	uint32_t lo = 0xFFFFFFFFu, hi = 0;
	for (c = 0; c < 10; c++) {
		uint32_t a = f->left[c], b = (uint32_t)(f->left[c] + f->width[c]) & 0xFF;
		lo = a < lo ? a : lo;
		hi = b > hi ? b : hi;
	}
	for (c = 0; c < 10; c++) {
		f->left[c] = (uint8_t)lo;
		f->width[c] = (uint8_t)(hi - lo);
	}
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

int font_from_mqf(struct font_entry *fe, const uint8_t *hdr, const uint8_t *body, uint32_t body_len, const char *name)
{
	if (be32(hdr) != 0x4D514631u)
		return 0;
	uint32_t lh = hdr[4], flags = hdr[6], nr = be16(hdr + 8), ng = be16(hdr + 10);
	if (!lh || body_len < 8 * nr + ng)
		return 0;
	const uint8_t *adv = body + 8 * nr, *cells = adv + ng;
	uint32_t cw = 1, g, r, k;

	for (g = 0; g < ng; g++)
		if (adv[g] > cw)
			cw = adv[g];
	uint32_t W = 16 * cw, H = 16 * lh;
	uint16_t *px = malloc(W * H * 2);
	if (!px)
		return 0;
	memset(px, 0, W * H * 2);
	uint8_t have[256];
	memset(have, 0, sizeof have);
	for (k = 0; k < 256; k++)
		fe->f.width[k] = 0;
	uint8_t width[256];
	memset(width, 0, sizeof width);
	for (r = 0; r < nr; r++) {
		const uint8_t *rg = body + 8 * r;
		uint32_t first = be32(rg), count = be16(rg + 4), g0 = be16(rg + 6), off = 0;

		for (g = 0; g < g0 && g < ng; g++)
			off += lh * ((adv[g] + 7u) >> 3);
		for (k = 0; k < count && g0 + k < ng; k++) {
			uint32_t c = first + k, a = adv[g0 + k], nb = (a + 7u) >> 3, x, y;

			if (c < 256 && 8 * nr + ng + off + lh * nb <= body_len) {
				uint32_t x0 = (c & 15) * cw, y0 = (c >> 4) * lh;
				for (y = 0; y < lh; y++)
					for (x = 0; x < a; x++)
						if (cells[off + y * nb + (x >> 3)] & (0x80u >> (x & 7)))
							px[(y0 + y) * W + x0 + x] = 0x7FFF;
				width[c] = (uint8_t)a;
				have[c] = 1;
			}
			off += lh * nb;
		}
	}
	struct image img = {ENGINE_FORMAT_RGB555, W, H, px, 0};
	font_build(&fe->f, 2, &img);
	if (!(flags & 2))                               /* plain font: flat advance = width, left 0, not proportional.
	                                                   Flag 2 (proportional grid): keep font_build's measuring. */
		for (k = 0; k < 256; k++) {
			fe->f.left[k] = 0;
			fe->f.width[k] = have[k] ? width[k] : 0;
			fe->f.right[k] = (uint8_t)(cw - fe->f.width[k]);
			fe->f.proportional[k] = 0;
		}
	for (k = 0; k < 255 && name && name[k]; k++)
		fe->name[k] = name[k];
	fe->name[k] = 0;
	return 1;
}

/* The picks walk font_table[0 .. font_count). fonts.c moves the fonts automatic picks may choose to the front and
 * lowers font_count while they run; without fonts.c, font_pick_largest / font_pick_for_text honour fonts_state
 * themselves by walking idx[]. */
static int32_t box_h_for(const struct rect *box, int mode)
{
	int32_t h = box->h;
	if (mode == 1 || mode == 2)
		return h - 2;
	return mode == 3 ? h - 1 : h;
}

static void pick_outputs(const struct rect *box, uint32_t w, uint32_t h, struct rect *r, struct rect *r2, struct point *pos)
{
	if (r)
		r->x = 0, r->y = 0, r->w = (int32_t)w, r->h = (int32_t)h;
	if (r2) {
		r2->x = 0, r2->y = 0;
		r2->w = (int32_t)((uint32_t)box->w < w ? (uint32_t)box->w : w);
		r2->h = (int32_t)((uint32_t)box->h < h ? (uint32_t)box->h : h);
		if (pos)
			pos->x = (box->w - r2->w) / 2 + box->x, pos->y = (box->h - r2->h) / 2 + box->y;
	}
}

static void pick_largest(struct font *out, const struct rect *box, struct rect *r, struct rect *r2, struct point *pos,
                         int mode, const uint8_t *idx, uint32_t n)
{
	uint32_t best_w = 0, best_h = 0, fb_w = 0, fb_h = 0xFFFFFFFFu, k;
	int32_t box_h = box_h_for(box, mode);
	int found = -1, fb = -1;

	for (k = 0; k < n; k++) {
		int i = idx ? idx[k] : (int)k;
		const struct font *f = &font_table[i].f;
		uint32_t cw = f->img.w >> 4, ch = font_cell_h(f);

		if (cw < best_w || box_h < (int32_t)ch || ch < best_h) {
			if (fb_w < cw && ch <= fb_h)
				fb = i, fb_h = ch, fb_w = cw;
		} else
			found = i, best_h = ch, best_w = cw;
	}
	if (found < 0)
		found = fb;
	if (found >= 0) {
		struct font *f = &font_table[found].f;
		if (out != f)
			*out = *f;
		pick_outputs(box, f->img.w >> 4, font_cell_h(f), r, r2, pos);
	}
}

static void pick_for_text(struct sbuf *text, struct font *out, const struct rect *box, struct rect *r, struct rect *r2,
                          struct point *pos, int mode, const uint8_t *idx, uint32_t n)
{
	uint32_t best_w = 0, best_h = 0, fb_w = 0xFFFFFFFFu, fb_h = 0xFFFFFFFFu, k;
	int32_t box_h = box_h_for(box, mode), box_w = box->w;
	int found = -1, fb = -1;

	for (k = 0; k < n; k++) {
		int i = idx ? idx[k] : (int)k;
		struct font *f = &font_table[i].f;
		uint32_t w = (uint32_t)text_width__orig(f, text), h = font_cell_h(f);

		if (box_w < (int32_t)w || w < best_w || box_h < (int32_t)h || h < best_h) {
			if (w <= fb_w && h <= fb_h)
				fb = i, fb_h = h, fb_w = w;
		} else
			found = i, best_h = h, best_w = w;
	}
	if (found < 0)
		found = fb;
	if (found >= 0) {
		struct font *f = &font_table[found].f;
		if (out != f)
			*out = *f;
		pick_outputs(box, (uint32_t)text_width__orig(f, text), font_cell_h(f), r, r2, pos);
	}
}

void font_pick_largest__orig(struct font *out, void *box, void *r, void *r2, void *pos, int mode)
{
	pick_largest(out, box, r, r2, pos, mode, 0, font_count);
}

void font_pick_for_text__orig(void *text, struct font *out, void *box, void *r, void *r2, void *pos, int a, int mode)
{
	(void)a;
	pick_for_text(text, out, box, r, r2, pos, mode, 0, font_count);
}

/* The default detour: fonts_state's list, without moving anything. fonts.c's strong ext_font_pick_* replace these. */
__attribute__((weak)) void ext_font_pick_largest(struct font *out, void *box, void *r, void *r2, void *pos, int mode)
{
	if (fonts_state.n)
		pick_largest(out, box, r, r2, pos, mode, fonts_state.idx, fonts_state.n);
	else
		font_pick_largest__orig(out, box, r, r2, pos, mode);
}

__attribute__((weak)) void ext_font_pick_for_text(void *text, struct font *out, void *box, void *r, void *r2, void *pos,
                                                  int a, int mode)
{
	if (fonts_state.n)
		pick_for_text(text, out, box, r, r2, pos, mode, fonts_state.idx, fonts_state.n);
	else
		font_pick_for_text__orig(text, out, box, r, r2, pos, a, mode);
}

void font_pick_largest(struct font *out, struct rect *box, struct rect *r, struct rect *r2, struct point *pos, int mode)
{
	ext_font_pick_largest(out, box, r, r2, pos, mode);
}

void font_pick_for_text(struct sbuf *text, struct font *out, struct rect *box, struct rect *r, struct rect *r2,
                        struct point *pos, int a, int mode)
{
	ext_font_pick_for_text(text, out, box, r, r2, pos, a, mode);
}
