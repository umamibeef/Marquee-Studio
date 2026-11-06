/* Fonts by name, in our own format (MQF1, studio/marquee_studio/mqf.py): after the stock fonts_load (fonte00..99.png,
 * the sign's own fonts), every font named in the archive file fonts.lst (one name per line) is loaded from its .mqf
 * file and appended to the firmware's font table under that name. The stock text engine then draws it like any other
 * font: each glyph is laid out in a 16 x 16 cell grid image (RGB555, ink 0x7FFF: what blit_mask_2colour reads), and
 * its metrics come from the font itself rather than from trimming ink. Every glyph is drawn exactly its designer's
 * advance wide: width = advance, not proportional (so the engine adds no separator columns). The web Studio builds
 * the same font in frontend/packages/sign-emu src/mqf.ts. */
#include <stdint.h>

struct image { uint32_t format, w, h; uint16_t *pixels; uint16_t *palette; };
struct font {
	uint32_t layout;
	struct image img;
	uint8_t left[256], width[256], right[256], proportional[256], digit_left[256], digit_width[256];
};
struct font_entry { struct font f; char name[256]; };     /* 0x718 bytes */
struct lfs_file { const uint8_t *data; uint32_t size, f8, flags, f10, pos, f18, f1c, f20, name, index, f2c; };

void *malloc(unsigned long size);
void free(void *p);
int uncompress(uint8_t *dst, uint32_t *dst_len, const uint8_t *src, uint32_t src_len);
int lfs_open(struct lfs_file *fh, const char *name);
void font_build(struct font *f, uint32_t layout, struct image *img);
extern struct font_entry font_table[100];
extern uint8_t font_count;

#define MQF_MAGIC 0x4D514631u                    /* "MQF1" */

/* Automatic picks. When text names no font, the firmware picks one itself: font_pick_largest (a line's font: the
 * widest that fits its height) and font_pick_for_text (a centred segment's: the largest that fits the text). Both
 * walk font_get 0, 1, 2 ... until font_count, and copy the font they pick into their output. The archive file
 * fonts.auto names the fonts those picks may choose (firmware/auto_fonts.txt, one name per line). While a pick runs,
 * those fonts are moved to the front of the font table (in table order, so ties go as they would) and font_count is
 * their number; afterwards the table is put back exactly. So the picks keep their own rules but never land on a
 * display font such as a blackletter; everything else numbers the fonts as before. (font_get itself cannot be taken
 * over: it starts with a branch.) No fonts.auto, or none of its fonts loaded: the stock behaviour. The state lives
 * at +0x60 of the extension state block (fwext.py), cleared at boot. */
#define AUTO_MAX 24
struct fonts_state { uint32_t picking, n; uint8_t idx[AUTO_MAX]; };
extern struct fonts_state fonts_state;
void font_pick_largest__orig(struct font *out, void *box, void *r, void *r2, void *pos, int mode);
void font_pick_for_text__orig(void *text, struct font *out, void *box, void *r, void *r2, void *pos, int a, int mode);

static void swap_fonts(unsigned a, unsigned b)
{
	uint32_t *x = (uint32_t *)&font_table[a], *y = (uint32_t *)&font_table[b];
	for (unsigned k = 0; k < sizeof(struct font_entry) / 4; k++) {
		uint32_t t = x[k];
		x[k] = y[k];
		y[k] = t;
	}
}

/* the auto fonts to the front (idx ascending, so position k <= idx[k]); returns the real font count */
static uint8_t auto_begin(void)
{
	uint8_t count = font_count;
	if (!fonts_state.n)
		return count;
	for (unsigned k = 0; k < fonts_state.n; k++)
		if (fonts_state.idx[k] != k)
			swap_fonts(k, fonts_state.idx[k]);
	font_count = (uint8_t)fonts_state.n;
	return count;
}

static void auto_end(uint8_t count)
{
	if (!fonts_state.n)
		return;
	for (unsigned k = fonts_state.n; k-- > 0;)
		if (fonts_state.idx[k] != k)
			swap_fonts(k, fonts_state.idx[k]);
	font_count = count;
}

void ext_font_pick_largest(struct font *out, void *box, void *r, void *r2, void *pos, int mode)
{
	uint8_t count = auto_begin();
	font_pick_largest__orig(out, box, r, r2, pos, mode);
	auto_end(count);
}

void ext_font_pick_for_text(void *text, struct font *out, void *box, void *r, void *r2, void *pos, int a, int mode)
{
	uint8_t count = auto_begin();
	font_pick_for_text__orig(text, out, box, r, r2, pos, a, mode);
	auto_end(count);
}

/* font_pick_largest's rule for a box_h-high line, over the fonts automatic picks may choose: the font number (the
 * apps' font_auto helper, apps/include/sign.h), -1 if there are no fonts */
int fonts_auto_pick(uint32_t box_h)
{
	uint32_t best_w = 0, best_h = 0, fb_w = 0, fb_h = 0xFFFFFFFFu, n = fonts_state.n ? fonts_state.n : font_count;
	int found = -1, fb = -1;
	for (uint32_t k = 0; k < n; k++) {
		unsigned i = fonts_state.n ? fonts_state.idx[k] : k;
		const struct font *f = &font_table[i].f;
		uint32_t cw = f->img.w >> 4, ch = f->layout == 1 ? f->img.h >> 3 : f->layout == 0 ? f->img.h : f->img.h >> 4;
		if (cw < best_w || box_h < ch || ch < best_h) {
			if (fb_w < cw && ch <= fb_h)
				fb = (int)i, fb_h = ch, fb_w = cw;
		} else
			found = (int)i, best_h = ch, best_w = cw;
	}
	return found >= 0 ? found : fb;
}

/* fonts.auto: the font table indices of the named fonts, ascending */
static void auto_load(void)
{
	struct lfs_file fa;
	if (!lfs_open(&fa, "fonts.auto"))
		return;
	const char *p = (const char *)fa.data, *end = p + fa.size;
	unsigned n = 0;
	while (p < end && n < AUTO_MAX) {
		const char *s = p;
		while (p < end && *p != '\n' && *p != '\r')
			p++;
		unsigned len = (unsigned)(p - s);
		while (p < end && (*p == '\n' || *p == '\r'))
			p++;
		if (!len || *s == '#')
			continue;
		for (unsigned i = 0; i < font_count; i++) {
			const char *nm = font_table[i].name;
			unsigned k = 0;
			while (k < len && nm[k] == s[k])
				k++;
			if (k == len && nm[k] == 0) {
				unsigned j = n;                      /* insert, keeping the indices ascending */
				while (j && fonts_state.idx[j - 1] > i) {
					fonts_state.idx[j] = fonts_state.idx[j - 1];
					j--;
				}
				fonts_state.idx[j] = (uint8_t)i;
				n++;
				break;
			}
		}
	}
	fonts_state.n = n;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

/* One MQF1 font into the next font_table slot; 0 if it is not a valid font or the table is full. */
static int mqf_load(const uint8_t *d, uint32_t size, const char *name, unsigned name_len)
{
	if (size < 16 || be32(d) != MQF_MAGIC || font_count >= 100)
		return 0;
	unsigned lh = d[4], flags = d[6], nr = be16(d + 8), ng = be16(d + 10);
	uint32_t blen = be32(d + 12), n = blen;
	uint8_t *body = malloc(blen ? blen : 1);
	if (!body)
		return 0;
	if (flags & 1) {
		if (uncompress(body, &n, d + 16, size - 16) || n != blen) {
			free(body);
			return 0;
		}
	} else {
		if (size - 16 < blen) {
			free(body);
			return 0;
		}
		for (uint32_t k = 0; k < blen; k++)
			body[k] = d[16 + k];
	}
	const uint8_t *adv = body + 8 * nr, *cells = adv + ng;
	unsigned cw = 1;
	for (unsigned g = 0; g < ng; g++)
		if (adv[g] > cw)
			cw = adv[g];
	uint32_t W = 16 * cw, H = 16 * lh;
	uint16_t *px = malloc(W * H * 2);
	if (!px || lh == 0) {
		free(body);
		return 0;
	}
	for (uint32_t k = 0; k < W * H; k++)
		px[k] = 0;
	uint8_t width[256], have[256];
	for (unsigned c = 0; c < 256; c++)
		width[c] = 0, have[c] = 0;
	/* glyph g's cell bytes start after all earlier glyphs' cells */
	const uint8_t *cp = cells;
	unsigned g = 0;
	for (unsigned r = 0; r < nr; r++) {
		const uint8_t *rg = body + 8 * r;
		uint32_t first = be32(rg), count = be16(rg + 4), g0 = be16(rg + 6);
		for (; g < g0; g++)                                   /* (ranges are in glyph order) */
			cp += lh * ((adv[g] + 7u) / 8u);
		for (uint32_t k = 0; k < count; k++, g++) {
			unsigned a = adv[g], nb = (a + 7u) / 8u;
			uint32_t c = first + k;
			if (c < 256) {
				uint32_t x0 = (c & 15) * cw, y0 = (c >> 4) * lh;
				for (unsigned y = 0; y < lh; y++)
					for (unsigned x = 0; x < a; x++)
						if (cp[y * nb + x / 8] & (0x80u >> (x % 8)))
							px[(y0 + y) * W + x0 + x] = 0x7FFF;
				width[c] = (uint8_t)a, have[c] = 1;
			}
			cp += lh * nb;
		}
	}
	free(body);
	struct font_entry *fe = &font_table[font_count];
	struct image img = {1, W, H, px, 0};
	font_build(&fe->f, 2, &img);
	if (!(flags & 2))                                        /* flag 2 (proportional grid): keep font_build's measuring
	                                                            (left bearing, 1 px gaps, space/digit rules); else the
	                                                            designer's flat advance = width, left 0, not proportional */
		for (unsigned c = 0; c < 256; c++) {
			fe->f.left[c] = 0;
			fe->f.width[c] = have[c] ? width[c] : 0;
			fe->f.right[c] = (uint8_t)(cw - fe->f.width[c]);
			fe->f.proportional[c] = 0;
		}
	unsigned k;
	for (k = 0; k < name_len && k < 255; k++)
		fe->name[k] = name[k];
	fe->name[k] = 0;
	font_count++;
	return 1;
}

void ext_fonts_load(void)
{
	struct lfs_file lst, f;
	if (!lfs_open(&lst, "fonts.lst"))
		return;
	const char *p = (const char *)lst.data, *end = p + lst.size;
	while (p < end) {
		const char *s = p;
		while (p < end && *p != '\n' && *p != '\r')
			p++;
		unsigned n = (unsigned)(p - s);
		while (p < end && (*p == '\n' || *p == '\r'))
			p++;
		if (!n || n > 120)
			continue;
		char name[128];
		for (unsigned k = 0; k < n; k++)
			name[k] = s[k];
		name[n] = 0;
		if (lfs_open(&f, name))
			mqf_load(f.data, f.size, name, n);
	}
	auto_load();
}
