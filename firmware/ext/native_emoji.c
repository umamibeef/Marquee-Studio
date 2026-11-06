/* Native emoji for ELITE_RGB: UTF-8 emoji in any text, drawn in colour from one shared set, with any font.
 *
 * The content carries one file, emoji.bin (the studio makes it, studio/marquee_studio/native_emoji.py): a 20-byte
 * header (struct emoji_bin), then one zlib stream that unpacks to
 *   the palette   colours x RGB555 (entry 0 is transparent and not drawn)
 *   the map       the emoji, one per line in cell order, as UTF-8 (no variation selector)
 *   the table     (EMJ2 only) count x 6 bytes, big-endian: first frame, frames, ms per frame
 *   the pixels    the frames, size x size bytes each, each a palette index (EMJ1: one frame per cell)
 * It is unpacked once, on first use, with the firmware's own zlib (uncompress) into a malloc'd buffer. The file is
 * taken from the content store (store.c, main flash) when it holds one, else from the boot flash's archive.
 *
 * Animated emoji. A message line's text is drawn once into its text image and then only blitted through the
 * line's effect, so an animated cell is put right at blit time: emoji_draw notes where it drew one (image, x, y,
 * cell; a ring of ANIM_RECS), and ext_glyph_blit_effect, for a source that still holds that cell's frame 0 there,
 * takes the cell out of the mask (so the line's background shows where the current frame is transparent), blits,
 * puts frame 0 back, and draws frame (t * 25 / ms) % frames in its own colours. t is the step count of the line on
 * screen, or the app's (emoji_anim_time, from apps.c); a step is 25 ms. sign-emu (native.ts) does the same.
 *
 * The text routines below are detoured here (analysis/tools/fwext.py links each ext_<name> in place of
 * <name> and makes <name>__orig run the original). A text with no emoji from the map goes straight to the
 * original, so every existing text, font and effect draws exactly as before. An emoji is recognised only as a
 * UTF-8 sequence that is in the map (they start with a byte >= 0xC2 followed by 0x80..0xBF bytes, which
 * Latin-1 sign text does not contain), and it counts as one proportional glyph as wide as a cell. The cells are
 * pixel art made at the display height and drawn 1:1.
 *
 * The state lives just past the C heap's old start (fwbuild moves the heap up by EXT_STATE_SIZE) and is cleared
 * by ext_boot_card at every start, since that memory is not zeroed. */
#include <stdint.h>

struct image { uint32_t format, w, h; uint16_t *pixels; uint16_t *palette; };
struct font {
	uint32_t layout;
	struct image img;
	uint8_t left[256], width[256], right[256], proportional[256];
};
struct sbuf { const uint8_t *data; uint16_t cap, len; };
struct point { int32_t x, y; };
struct rect { int32_t x, y, w, h; };
/* 48 bytes: lfs_find_in_volume also fills +0x20 (name length), +0x24 (name) and +0x28 (index) */
struct lfs_file { const uint8_t *data; uint32_t size, f8, flags, f10, pos, f18, f1c, f20, name, index, f2c; };

struct anim_rec { const uint16_t *img; int16_t x, y; uint16_t cell, pad; };
#define ANIM_RECS 32

struct emoji_state {                            /* the first 7 words are read by console.c too */
	uint32_t tried;
	uint32_t size, count;
	const uint16_t *palette;
	const uint8_t *pixels;
	const uint8_t *map;
	uint32_t map_len;
	const uint8_t *table;                       /* EMJ2's cell table, 0 for EMJ1 */
	struct anim_rec *recs;                      /* ANIM_RECS, when the set has an animated cell */
	uint32_t rec_next, t;
	uint8_t *raw;
	struct motion *motion;                      /* per-character motion (below), allocated on first use */
};
struct emoji_bin { uint32_t magic; uint16_t size, count, colours, frames; uint32_t map_len, raw_len; };
#define EMOJI_MAGIC1 0x454D4A31                 /* "EMJ1" */
#define EMOJI_MAGIC  0x454D4A32                 /* "EMJ2" */
struct emoji_state emoji_state;

/* Firmware routines (addresses from analysis/symbols.tsv) */
void *malloc(unsigned long size);
void free(void *p);
int uncompress(uint8_t *dst, uint32_t *dst_len, const uint8_t *src, uint32_t src_len);
int lfs_open(struct lfs_file *fh, const char *name);
int store_find(const char *name, const uint8_t **data, uint32_t *size);
void draw_line(struct image *dst, struct point *from, struct point *to, uint32_t colour);
void font_draw_char_part(struct image *dst, struct point *pos, struct font *f, uint32_t ch, struct rect *part,
                         uint32_t fg, uint32_t bg);
void font_draw_cell_part(struct image *dst, struct point *pos, struct font *f, uint32_t ch, struct rect *part);
void glyph_blit_effect_tex(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos, int32_t col, int32_t row);
void glyph_blit_effect__orig(struct image *src, struct image *dst, uint32_t effect, struct rect *r,
                             struct point *pos);
void font_draw_char_effect(struct image *dst, struct point *pos, struct point *fg_origin, struct point *bg_origin,
                           struct font *f, uint32_t ch, uint32_t phase, struct rect *part, uint32_t effect);
int text_width__orig(struct font *f, struct sbuf *t);
void text_draw_proportional__orig(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                  uint16_t *colours, uint16_t *width_out);
void text_draw_proportional_plain__orig(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                        int16_t *width_out);
void text_draw_proportional_clip_maybe__orig(struct image *dst, struct point *pos, struct point *fg_origin,
                                             struct point *bg_origin, uint32_t unused, struct font *f,
                                             struct sbuf *t, const uint32_t *effects, uint32_t n_effects,
                                             uint32_t unused2, int16_t *width_out);

#define VS16_0 0xEF                             /* U+FE0F, an emoji presentation selector: EF B8 8F */

static __attribute__((noinline)) void emoji_load(void)
{
	struct lfs_file fh;
	const struct emoji_bin *h;
	const uint8_t *data;
	uint32_t size, n, c;
	uint8_t *raw;

	emoji_state.tried = 1;
	if (!store_find("emoji.bin", &data, &size)) {
		if (!lfs_open(&fh, "emoji.bin"))
			return;
		data = fh.data, size = fh.size;
	}
	if (size <= sizeof *h)
		return;
	h = (const struct emoji_bin *)data;
	if ((h->magic != EMOJI_MAGIC && h->magic != EMOJI_MAGIC1) || !(raw = malloc(n = h->raw_len)))
		return;
	if (uncompress(raw, &n, (const uint8_t *)(h + 1), size - sizeof *h) || n != h->raw_len) {
		free(raw);
		return;
	}
	emoji_state.raw = raw;
	emoji_state.size = h->size;
	emoji_state.count = h->count;
	emoji_state.palette = (const uint16_t *)raw;
	emoji_state.map = raw + h->colours * 2;
	emoji_state.map_len = h->map_len;
	emoji_state.pixels = emoji_state.map + h->map_len;
	if (h->magic == EMOJI_MAGIC) {
		emoji_state.table = emoji_state.pixels;
		emoji_state.pixels += h->count * 6;
		for (c = 0; c < h->count; c++)
			if ((emoji_state.table[c * 6 + 2] << 8 | emoji_state.table[c * 6 + 3]) > 1) {
				emoji_state.recs = malloc(ANIM_RECS * sizeof(struct anim_rec));
				for (n = 0; emoji_state.recs && n < ANIM_RECS; n++)
					emoji_state.recs[n].img = 0;
				break;
			}
	}
}

/* After the content store changes: the next emoji is read from the new emoji.bin. */
void emoji_reset(void)
{
	if (emoji_state.raw)
		free(emoji_state.raw);
	if (emoji_state.recs)
		free(emoji_state.recs);
	emoji_state.raw = 0, emoji_state.recs = 0, emoji_state.table = 0, emoji_state.map = 0;
	emoji_state.map_len = 0, emoji_state.count = 0, emoji_state.rec_next = 0;
	emoji_state.tried = 0;
}

/* The clock of animated emoji: the step count of the line (or app) being drawn (apps.c). */
void emoji_anim_time(uint32_t t)
{
	emoji_state.t = t;
}

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* A cell's first frame, its frames and ms per frame (EMJ1: frame `cell`, a still). */
static unsigned cell_first(unsigned cell) { return emoji_state.table ? be16(emoji_state.table + cell * 6) : cell; }
static unsigned cell_frames(unsigned cell) { return emoji_state.table ? be16(emoji_state.table + cell * 6 + 2) : 1; }
static unsigned cell_ms(unsigned cell) { return emoji_state.table ? be16(emoji_state.table + cell * 6 + 4) : 0; }

static const uint8_t *frame_px(unsigned cell, unsigned k)
{
	unsigned s = emoji_state.size;
	return emoji_state.pixels + (cell_first(cell) + k) * s * s;
}

/* The longest map entry that s[0..n) starts with: its length in bytes (plus a trailing U+FE0F, if any), and its
 * cell in *cell. 0 if none. */
static unsigned emoji_match(const uint8_t *s, unsigned n, unsigned *cell)
{
	const uint8_t *m, *end;
	unsigned best = 0, idx = 0;

	if (s[0] < 0xC2)
		return 0;
	if (!emoji_state.tried)
		emoji_load();
	m = emoji_state.map;
	end = m + emoji_state.map_len;
	while (m && m < end) {
		const uint8_t *line = m;
		unsigned k = 0;

		while (m < end && *m != '\n')
			m++;
		while (line + k < m && k < n && line[k] == s[k])
			k++;
		if (line + k == m && k > best) {
			best = k;
			*cell = idx;
		}
		m++;
		idx++;
	}
	if (best && best + 3 <= n && s[best] == VS16_0 && s[best + 1] == 0xB8 && s[best + 2] == 0x8F)
		best += 3;
	return best;
}

/* Per-character effects: the in-text escape 0x1F (ASCII US) starts a command inside a segment (the stock tokenizer
 * ends a segment only at CR, ESC and NUL, so these stay with their letters); its arguments are printable, so a
 * command never holds NUL, CR or ESC. A command takes no room and applies to the characters after it:
 *   0x1F 'm' d          motion d: 0 none, 1 wave, 2 bounce, 3 shake, 4 blink
 *   0x1F 'f' hhhh       fill: the letters' effect word (RGB555, 8001 wipe, 8002 stripes, C0nn animation slot nn)
 *   0x1F 'f' '-'        fill: the line's own again
 *   0x1F 'o' hh hh      on / off time in tenths of a second: the effects run while on and rest while off
 *                       (blink: its rhythm); on 00: always on
 * Anything else after 0x1F: the 0x1F alone is skipped. Returns the command's length at s (0: not 0x1F), and the
 * command in *cmd ('m', 'f', 'o', or 0 for a lone 0x1F) and its value in *val (fill reset: 0x10000). */
static int hexv(uint8_t c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static unsigned fx_cmd(const uint8_t *s, unsigned n, unsigned *cmd, uint32_t *val)
{
	unsigned k;
	int v;

	*cmd = 0, *val = 0;
	if (!n || s[0] != 0x1F)
		return 0;
	if (n >= 3 && s[1] == 'm' && s[2] >= '0' && s[2] <= '4') {
		*cmd = 'm', *val = s[2] - '0';
		return 3;
	}
	if (n >= 3 && s[1] == 'f' && s[2] == '-') {
		*cmd = 'f', *val = 0x10000;
		return 3;
	}
	if (n >= 6 && (s[1] == 'f' || s[1] == 'o')) {
		uint32_t w = 0;
		for (k = 2; k < 6; k++) {
			if ((v = hexv(s[k])) < 0)
				return 1;
			w = w << 4 | (uint32_t)v;
		}
		*cmd = s[1], *val = w;
		return 6;
	}
	return 1;
}

/* a text with an emoji of the map or an in-text effect command: drawn by us (else by the original, as before) */
static int has_emoji(const struct sbuf *t)
{
	unsigned i, cell;

	for (i = 0; i < t->len; i++)
		if (t->data[i] == 0x1F || (t->data[i] >= 0xC2 && emoji_match(t->data + i, t->len - i, &cell)))
			return 1;
	return 0;
}

static unsigned cell_height(const struct font *f)
{
	if (f->layout == 1)
		return f->img.h >> 3;
	if (f->layout == 0)
		return f->img.h;
	return f->img.h >> 4;
}

/* Draws cell `cell` 1:1 (no scaling: the cells are made at the display height) with its left edge at x, centred
 * vertically on the h-pixel text line at y (it can reach above and below a shorter font; the destination clips
 * it). Palette entry 0 is transparent; every other pixel is an opaque RGB555 colour. studio/marquee_studio/
 * native_emoji.py draws the same way for the preview. */
#define TEXT_IMG_W 0x1414                       /* the text image of a message line (textline_init) */
#define EMOJI_TAG 0x8000                        /* bit 15 of an RGB555 pixel: "an emoji's own colour" */

static __attribute__((noinline)) void emoji_draw(struct image *dst, int x, int y, unsigned cell, unsigned h)
{
	unsigned s = emoji_state.size, dx, dy;
	const uint8_t *src = frame_px(cell, 0);
	/* A message line's text image reaches the panel only as the mask of glyph_blit_effect, which would paint the
	 * emoji in the line's fill: its emoji pixels are tagged, and ext_glyph_blit_effect puts their colours back. */
	uint16_t tag = dst->w == TEXT_IMG_W ? EMOJI_TAG : 0;

	y += ((int)h - (int)s) / 2;                     /* C division: rounds toward zero */
	if (tag && emoji_state.recs && cell_frames(cell) > 1) {   /* an animated cell: noted for the blit */
		struct anim_rec *r = 0;
		unsigned k;

		for (k = 0; k < ANIM_RECS && !r; k++)
			if (emoji_state.recs[k].img == dst->pixels && emoji_state.recs[k].x == x && emoji_state.recs[k].y == y)
				r = &emoji_state.recs[k];
		if (!r)
			r = &emoji_state.recs[emoji_state.rec_next++ % ANIM_RECS];
		r->img = dst->pixels, r->x = (int16_t)x, r->y = (int16_t)y, r->cell = (uint16_t)cell;
	}
	for (dy = 0; dy < s; dy++, src += s) {
		int py = y + (int)dy;

		if (py < 0 || py >= (int)dst->h)
			continue;
		for (dx = 0; dx < s; dx++) {
			int px = x + (int)dx;

			if (src[dx] && px >= 0 && px < (int)dst->w)
				dst->pixels[py * dst->w + px] = emoji_state.palette[src[dx]] | tag;
		}
	}
}

/* One glyph of a text: a font character, or an emoji (cell, bytes it spans). */
struct glyph { unsigned ch, emoji, cell, bytes, width, prop, cmd; uint32_t val; };

static __attribute__((noinline)) void next_glyph(const struct font *f, const struct sbuf *t, unsigned i, struct glyph *g)
{
	unsigned c = t->data[i];

	g->cmd = 0, g->val = 0;
	if (c == 0x1F) {                                /* an effect command: no glyph, no room */
		g->bytes = fx_cmd(t->data + i, t->len - i, &g->cmd, &g->val);
		g->emoji = 0, g->ch = ' ', g->width = 0, g->prop = 0;
		if (!g->cmd)
			g->cmd = 1;                             /* a lone 0x1F: skipped */
		return;
	}
	g->bytes = c >= 0xC2 ? emoji_match(t->data + i, t->len - i, &g->cell) : 0;
	g->emoji = g->bytes != 0;
	if (g->emoji) {
		g->ch = ' ';
		g->width = emoji_state.size;
		g->prop = 1;
	} else {
		g->ch = c;
		g->bytes = 1;
		g->width = f->width[c];
		g->prop = f->proportional[c];
	}
}

int ext_text_width(struct font *f, struct sbuf *t)
{
	unsigned i = 0, prev = 1;
	int w = 0;
	struct glyph g;

	if (!has_emoji(t))
		return text_width__orig(f, t);
	while (i < t->len) {
		next_glyph(f, t, i, &g);
		if (g.cmd) {
			i += g.bytes;
			continue;
		}
		w += g.prop ? (prev == 0) + g.width + 1 : g.width;
		prev = g.prop;
		i += g.bytes;
	}
	return w;
}

/* ---- per-character motion (wave, bounce, shake, blink) ----
 * A text image is drawn once per segment and then only blitted, so motion is applied at blit time: while a text is
 * drawn into a text image (TEXT_IMG_W wide), each glyph after an effect command (0x1F) is noted as a span (its columns, its
 * motion, its index in the text); ext_glyph_blit_effect then blits a copy of the source rect with each span's
 * columns displaced for this step (t, as animated emoji). The same rules, in the same integer arithmetic, are
 * sign-emu's (native.ts motionOffset). */
#define MSPANS  96
#define MTABLES 4
struct mspan { int16_t x0, x1; uint8_t kind, ci, has_fill, on, off, pad; uint16_t fill; };
/* the effects in force while a text is drawn (its commands so far) */
struct fxstate { unsigned kind, has_fill, fill, on, off; };
struct mtable { const uint16_t *img; uint32_t n; struct mspan s[MSPANS]; };
struct motion { struct mtable t[MTABLES]; uint32_t next; uint16_t *tmp; uint32_t tmp_size; };

static const int8_t WAVE[64] = {0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
                                1, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
                                -2, -2, -2, -2, -1, -1, -1, -1, -1, -1, 0, 0};
static const int8_t HOP[32] = {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 1, 1,
                               1, 1, 0};

enum { M_NONE, M_WAVE, M_BOUNCE, M_SHAKE, M_BLINK };

/* the displacement of column sx of glyph ci with motion kind at step t; 0 when the column is hidden (blink off).
 * sx counts from the text's first effect letter, so it moves with the letters: from a panel column, a wave scrolling
 * one pixel a step would cancel out and freeze. */
static int motion_offset(unsigned kind, int sx, unsigned ci, uint32_t t, int *dx, int *dy)
{
	*dx = *dy = 0;
	switch (kind) {
	case M_WAVE:
		*dy = WAVE[(uint32_t)(sx * 2 + (int)t * 2) & 63];
		return 1;
	case M_BOUNCE: {
		uint32_t p = (uint32_t)((int)t * 2 + 512 - (int)ci * 6) & 63;
		*dy = p < 32 ? -HOP[p] : 0;
		return 1;
	}
	case M_SHAKE: {
		uint32_t h = (uint32_t)((ci * 31u + (t / 3u) * 977u + 1u) * 2654435761u) >> 16;
		*dx = (int)(h % 3u) - 1;
		*dy = (int)(h / 3u % 3u) - 1;
		return 1;
	}
	case M_BLINK:
		return ((t / 12u) & 1u) == 0;
	default:
		return 1;
	}
}

static struct mtable *mtable_of(const uint16_t *img, int create)
{
	struct motion *m = emoji_state.motion;
	unsigned k;

	if (!m && create) {
		m = malloc(sizeof *m);
		if (!m)
			return 0;
		for (k = 0; k < MTABLES; k++)
			m->t[k].img = 0, m->t[k].n = 0;
		m->next = 0, m->tmp = 0, m->tmp_size = 0;
		emoji_state.motion = m;
	}
	if (!m)
		return 0;
	for (k = 0; k < MTABLES; k++)
		if (m->t[k].img == img)
			return &m->t[k];
	if (!create)
		return 0;
	struct mtable *t = &m->t[m->next++ % MTABLES];
	t->img = img, t->n = 0;
	return t;
}

/* a new text is being drawn into dst: its old spans no longer apply */
static void motion_reset(const struct image *dst)
{
	struct mtable *t = mtable_of(dst->pixels, 0);

	if (t)
		t->n = 0;
}

static void fx_apply(struct fxstate *e, const struct glyph *g)
{
	if (g->cmd == 'm')
		e->kind = g->val;
	else if (g->cmd == 'f')
		e->has_fill = g->val != 0x10000, e->fill = g->val & 0xFFFF;
	else if (g->cmd == 'o')
		e->on = g->val >> 8, e->off = g->val & 0xFF;
}

/* image_blit copies a text image into a message line's image as it slides (textline_step): the copy's per-letter
 * effects go with it, moved by the copy's offset, so the line image's blit onto the panel applies them. A copy of
 * a text without effects leaves none. */
void image_blit__orig(struct image *src, struct image *dst, struct rect *r, struct point *pos);

void ext_image_blit(struct image *src, struct image *dst, struct rect *r, struct point *pos)
{
	struct rect rc = *r;
	struct point at = *pos;
	struct mtable *from = src->pixels ? mtable_of(src->pixels, 0) : 0;

	image_blit__orig(src, dst, r, pos);
	if (!dst->pixels || dst == src)
		return;
	if (!from || !from->n) {
		motion_reset(dst);
		return;
	}
	struct mtable *to = mtable_of(dst->pixels, 1);
	from = mtable_of(src->pixels, 0);                   /* (creating dst's table may have reused src's slot) */
	if (!to || !from || to == from)
		return;
	int off = at.x - rc.x;
	unsigned k;
	to->n = 0;
	for (k = 0; k < from->n && to->n < MSPANS; k++) {
		const struct mspan *sp = &from->s[k];
		int x0 = sp->x0 + off, x1 = sp->x1 + off;

		if (sp->x1 <= rc.x || sp->x0 >= rc.x + rc.w)
			continue;
		x0 = x0 < at.x ? at.x : x0, x1 = x1 > at.x + rc.w ? at.x + rc.w : x1;
		x0 = x0 < 0 ? 0 : x0, x1 = x1 > (int)dst->w ? (int)dst->w : x1;
		if (x1 <= x0)
			continue;
		to->s[to->n] = *sp;
		to->s[to->n].x0 = (int16_t)x0, to->s[to->n].x1 = (int16_t)x1;
		to->n++;
	}
}

/* apps.c: the app's mask is cleared after each text it paints, and reused for its rectangles: its spans go too */
void emoji_effects_forget(const struct image *img)
{
	motion_reset(img);
}

static void motion_note(struct mtable *t, int x0, int x1, const struct fxstate *e, unsigned ci)
{
	if (!t || (e->kind == M_NONE && !e->has_fill && !e->on) || x1 <= x0 || t->n >= MSPANS)
		return;
	struct mspan *sp = &t->s[t->n++];
	sp->x0 = (int16_t)x0, sp->x1 = (int16_t)x1, sp->kind = (uint8_t)e->kind, sp->ci = (uint8_t)ci;
	sp->has_fill = (uint8_t)e->has_fill, sp->fill = (uint16_t)e->fill, sp->on = (uint8_t)e->on, sp->off = (uint8_t)e->off;
}

/* a span's effects run at step t: always without an on/off time; else during the first on tenths of each
 * on + off tenths (a step is 25 ms, a tenth 4 steps) */
static int span_on(const struct mspan *sp, uint32_t t)
{
	uint32_t period = ((uint32_t)sp->on + sp->off) * 4u;

	return !sp->on || !period || t % period < (uint32_t)sp->on * 4u;
}

/* The shared loop of the two plain proportional drawers: colours == 0 draws cells in their own colours
 * (text_draw_proportional_plain), else in colours[0] / colours[1]. */
static __attribute__((noinline)) void prop_glyph(struct image *dst, struct point *top, struct font *f,
                                                 const struct glyph *g, unsigned h, uint16_t *colours)
{
	struct rect part = { f->left[g->ch], 0, (int32_t)g->width, (int32_t)h };

	if (g->emoji)
		emoji_draw(dst, top->x, top->y, g->cell, h);
	else if (colours)
		font_draw_char_part(dst, top, f, g->ch, &part, colours[0], colours[1]);
	else
		font_draw_cell_part(dst, top, f, g->ch, &part);
}

static int draw_prop(struct image *dst, struct point *pos, struct font *f, struct sbuf *t, uint16_t *colours)
{
	unsigned i = 0, prev = 1, h = cell_height(f), ci = 0;
	struct fxstate e = {0, 0, 0, 0, 0};
	uint32_t bg = colours ? colours[1] : 0;
	struct point top = *pos, bottom = { pos->x, pos->y + (int32_t)h };
	struct glyph g;
	struct mtable *mt = dst->w == TEXT_IMG_W ? mtable_of(dst->pixels, 1) : 0;

	if (mt)
		mt->n = 0;
	while (i < t->len) {
		next_glyph(f, t, i, &g);
		if (g.cmd) {                                    /* an effect command: for the glyphs after it */
			fx_apply(&e, &g);
			i += g.bytes;
			continue;
		}
		if (!prev && g.prop) {
			draw_line(dst, &top, &bottom, 0);
			top.x++, bottom.x++;
		}
		int x0 = top.x;
		prop_glyph(dst, &top, f, &g, h, colours);
		top.x += g.width, bottom.x += g.width;
		if (g.prop) {
			draw_line(dst, &top, &bottom, bg);
			top.x++, bottom.x++;
		}
		motion_note(mt, x0, top.x, &e, ci++);
		prev = g.prop;
		i += g.bytes;
	}
	return top.x - pos->x;
}

void ext_text_draw_proportional(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                uint16_t *colours, uint16_t *width_out)
{
	if (!has_emoji(t)) {
		motion_reset(dst);
		text_draw_proportional__orig(dst, pos, f, t, colours, width_out);
		return;
	}
	*width_out = (uint16_t)draw_prop(dst, pos, f, t, colours);
}

void ext_text_draw_proportional_plain(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                      int16_t *width_out)
{
	if (!has_emoji(t)) {
		motion_reset(dst);
		text_draw_proportional_plain__orig(dst, pos, f, t, width_out);
		return;
	}
	*width_out = (int16_t)draw_prop(dst, pos, f, t, 0);
}

/* A one-pixel separator column (a space drawn with the effect in force), as the firmware draws it. */
static __attribute__((noinline)) void clip_sep(struct image *dst, struct point *at, struct point *fg_origin,
                                               struct point *bg_origin, struct font *f, unsigned i, unsigned h,
                                               uint32_t eff)
{
	struct rect col = { 0, 0, 1, (int32_t)h };

	font_draw_char_effect(dst, at, fg_origin, bg_origin, f, ' ', i, &col, eff);
	at->x++;
}

/* Message slots: each character position has its own effect word (effects[i], the last one repeated); the
 * separator columns are a space drawn with the effect in force. */
static __attribute__((noinline)) int16_t clip_text(struct image *dst, struct point *pos, struct point *fg_origin,
                                                    struct point *bg_origin, struct font *f, struct sbuf *t,
                                                    const uint32_t *effects, uint32_t n_effects)
{
	unsigned i = 0, prev = 1, h = cell_height(f), ci = 0;
	struct fxstate e = {0, 0, 0, 0, 0};
	uint32_t eff = 0x7FE00000;
	struct point at = *pos;
	struct glyph g;
	struct mtable *mt = dst->w == TEXT_IMG_W ? mtable_of(dst->pixels, 1) : 0;

	if (mt)
		mt->n = 0;
	while (i < t->len) {
		next_glyph(f, t, i, &g);
		if (g.cmd) {
			fx_apply(&e, &g);
			i += g.bytes;
			continue;
		}
		if (!prev && g.prop)
			clip_sep(dst, &at, fg_origin, bg_origin, f, i, h, eff);
		int x0 = at.x;
		eff = i < n_effects ? effects[i] : n_effects ? effects[n_effects - 1] : 0x7FE00000;
		if (g.emoji) {
			emoji_draw(dst, at.x, at.y, g.cell, h);
		} else {
			struct rect part = { f->left[g.ch], 0, (int32_t)g.width, (int32_t)h };

			font_draw_char_effect(dst, &at, fg_origin, bg_origin, f, g.ch, i, &part, eff);
		}
		at.x += g.width;
		if (g.prop)
			clip_sep(dst, &at, fg_origin, bg_origin, f, i, h, eff);
		motion_note(mt, x0, at.x, &e, ci++);
		prev = g.prop;
		i += g.bytes;
	}
	return (int16_t)(at.x - pos->x);
}

void ext_text_draw_proportional_clip_maybe(struct image *dst, struct point *pos, struct point *fg_origin,
                                           struct point *bg_origin, uint32_t unused, struct font *f,
                                           struct sbuf *t, const uint32_t *effects, uint32_t n_effects,
                                           uint32_t unused2, int16_t *width_out)
{
	if (!has_emoji(t)) {
		motion_reset(dst);
		text_draw_proportional_clip_maybe__orig(dst, pos, fg_origin, bg_origin, unused, f, t, effects,
		                                        n_effects, unused2, width_out);
	} else
		*width_out = clip_text(dst, pos, fg_origin, bg_origin, f, t, effects, n_effects);
}

/* Does src still hold record r's frame 0 (tagged) at its place? */
static __attribute__((noinline)) int rec_live(const struct image *src, const struct anim_rec *r)
{
	unsigned s = emoji_state.size, dx, dy;
	const uint8_t *f0 = frame_px(r->cell, 0);

	if (r->img != src->pixels || r->cell >= emoji_state.count)
		return 0;
	for (dy = 0; dy < s; dy++)
		for (dx = 0; dx < s; dx++) {
			int px = r->x + (int)dx, py = r->y + (int)dy;

			if (f0[dy * s + dx] && px >= 0 && px < (int)src->w && py >= 0 && py < (int)src->h &&
			    src->pixels[py * src->w + px] != (emoji_state.palette[f0[dy * s + dx]] | EMOJI_TAG))
				return 0;
		}
	return 1;
}

/* Writes record r's frame 0 into src (tagged), or clears its pixels (put = 0). */
static __attribute__((noinline)) void rec_put(struct image *src, const struct anim_rec *r, int put)
{
	unsigned s = emoji_state.size, dx, dy;
	const uint8_t *f0 = frame_px(r->cell, 0);

	for (dy = 0; dy < s; dy++)
		for (dx = 0; dx < s; dx++) {
			int px = r->x + (int)dx, py = r->y + (int)dy;

			if (f0[dy * s + dx] && px >= 0 && px < (int)src->w && py >= 0 && py < (int)src->h)
				src->pixels[py * src->w + px] = put ? emoji_state.palette[f0[dy * s + dx]] | EMOJI_TAG : 0;
		}
}

/* The current frame of record r over dst, for the source rect s placed at `at`. */
static __attribute__((noinline)) void rec_frame(struct image *dst, const struct anim_rec *r, const struct rect *sr,
                                                const struct point *at)
{
	unsigned s = emoji_state.size, n = cell_frames(r->cell), ms = cell_ms(r->cell), dx, dy;
	const uint8_t *f = frame_px(r->cell, ms ? emoji_state.t * 25 / ms % n : 0);

	for (dy = 0; dy < s; dy++)
		for (dx = 0; dx < s; dx++) {
			int sx = r->x + (int)dx, sy = r->y + (int)dy, x = at->x + sx - sr->x, y = at->y + sy - sr->y;

			if (f[dy * s + dx] && sx >= sr->x && sx < sr->x + sr->w && sy >= sr->y && sy < sr->y + sr->h &&
			    x >= 0 && x < (int)dst->w && y >= 0 && y < (int)dst->h)
				dst->pixels[y * dst->w + x] = emoji_state.palette[f[dy * s + dx]];
		}
}

/* Record r's current frame (tagged), over its frame 0's pixels which the caller cleared: the motion path below
 * blits a copy of the source, so an animated emoji goes in at its current frame. */
static __attribute__((noinline)) void rec_put_now(struct image *src, const struct anim_rec *r)
{
	unsigned s = emoji_state.size, n = cell_frames(r->cell), ms = cell_ms(r->cell), dx, dy;
	const uint8_t *f = frame_px(r->cell, ms ? emoji_state.t * 25 / ms % n : 0);

	for (dy = 0; dy < s; dy++)
		for (dx = 0; dx < s; dx++) {
			int px = r->x + (int)dx, py = r->y + (int)dy;

			if (f[dy * s + dx] && px >= 0 && px < (int)src->w && py >= 0 && py < (int)src->h)
				src->pixels[py * src->w + px] = emoji_state.palette[f[dy * s + dx]] | EMOJI_TAG;
		}
}

/* the record's cell area without its emoji pixels (tagged), before frame 0 goes back */
static __attribute__((noinline)) void rec_clear_box(struct image *src, const struct anim_rec *r)
{
	unsigned s = emoji_state.size, dx, dy;

	for (dy = 0; dy < s; dy++)
		for (dx = 0; dx < s; dx++) {
			int px = r->x + (int)dx, py = r->y + (int)dy;

			if (px >= 0 && px < (int)src->w && py >= 0 && py < (int)src->h && (src->pixels[py * src->w + px] & EMOJI_TAG))
				src->pixels[py * src->w + px] = 0;
		}
}

/* glyph_blit_effect for a source rect with moving characters: a copy of the rect with each span's columns displaced
 * for this step (the text image itself is left as drawn), blitted and then its emoji colours put back. 0 when no
 * span of src is in the rect (the caller then blits as before). */
#define NO_SHIFT 0x7F7Fu                             /* never a displacement: columns are at most 2 or 3 off */
static __attribute__((noinline)) int motion_blit(struct image *src, struct image *dst, uint32_t effect,
                                                 const struct rect *s, const struct point *at)
{
	struct mtable *mt = src->pixels && src->format == 1 ? mtable_of(src->pixels, 0) : 0;
	unsigned k, moving = 0;
	int x, y;

	if (!mt || s->w <= 0 || s->h <= 0)
		return 0;
	for (k = 0; k < mt->n && !moving; k++)
		moving = mt->s[k].x1 > s->x && mt->s[k].x0 < s->x + s->w;
	if (!moving)
		return 0;
	struct motion *m = emoji_state.motion;
	uint32_t need = (uint32_t)s->w * (uint32_t)s->h + (uint32_t)s->w * 3 + 2;   /* the copy, then a word and a shift per column */
	if (m->tmp_size < need) {
		if (m->tmp)
			free(m->tmp);
		m->tmp = malloc(need * 2);
		m->tmp_size = m->tmp ? need : 0;
		if (!m->tmp)
			return 0;
	}
	uint32_t *words = (uint32_t *)(m->tmp + (((uint32_t)s->w * (uint32_t)s->h + 1) & ~1u));
	uint16_t *shift = (uint16_t *)(words + s->w);    /* a letter's column: dx << 8 | dy (bytes), so its fill goes with it */
	uint32_t live = 0;                               /* animated emoji: their current frame goes into the copy */
	if (emoji_state.recs)
		for (k = 0; k < ANIM_RECS; k++)
			if (emoji_state.recs[k].img == src->pixels && rec_live(src, &emoji_state.recs[k])) {
				live |= 1u << k;
				rec_put(src, &emoji_state.recs[k], 0);
				rec_put_now(src, &emoji_state.recs[k]);
			}
	uint16_t *tmp = m->tmp;
	unsigned sp = 0;
	for (x = 0; x < s->w; x++) {
		int sx = s->x + x;
		const struct mspan *span = 0;

		for (k = sp; k < mt->n; k++)                 /* spans are noted left to right */
			if (sx >= mt->s[k].x0 && sx < mt->s[k].x1) {
				span = &mt->s[k];
				sp = k;
				break;
			}
		if (!span)
			for (k = 0; k < sp && !span; k++)
				if (sx >= mt->s[k].x0 && sx < mt->s[k].x1)
					span = &mt->s[k];
		int dx = 0, dy = 0, vis = 1;
		if (span) {
			int on = span_on(span, emoji_state.t);
			if (span->kind == M_BLINK && span->on)   /* blink with an on/off time: that is its rhythm */
				vis = on;
			else if (on)
				vis = motion_offset(span->kind, sx - mt->s[0].x0, span->ci, emoji_state.t, &dx, &dy);
		}
		words[x] = span && span->has_fill && span_on(span, emoji_state.t) ? (uint32_t)span->fill << 16 | (effect & 0xFFFF)
		                                                                   : effect;
		shift[x] = span ? (uint16_t)((uint8_t)dx << 8 | (uint8_t)dy) : NO_SHIFT;
		for (y = 0; y < s->h; y++) {
			int fx = sx - dx, fy = s->y + y - dy;
			uint16_t p = 0;

			if (vis && fx >= 0 && fx < (int)src->w && fy >= 0 && fy < (int)src->h &&
			    (!span || (fx >= span->x0 && fx < span->x1)))
				p = src->pixels[fy * src->w + fx];
			tmp[y * s->w + x] = p;
		}
	}
	struct image ti = { src->format, (uint32_t)s->w, (uint32_t)s->h, tmp, src->palette };
	for (x = 0; x < s->w;) {                         /* columns of one effect word: one blit */
		int x1 = x + 1;
		while (x1 < s->w && words[x1] == words[x] && shift[x1] == shift[x])
			x1++;
		struct rect r2 = { x, 0, x1 - x, s->h };
		struct point p2 = { at->x + x, at->y };
		if (shift[x] == NO_SHIFT)
			glyph_blit_effect__orig(&ti, dst, words[x], &r2, &p2);
		else                                         /* the texture from where the letter's pixels came from */
			glyph_blit_effect_tex(&ti, dst, words[x], &r2, &p2, s->x + x - (int8_t)(shift[x] >> 8) - mt->s[0].x0, -(int8_t)shift[x]);
		x = x1;
	}
	if (dst->pixels)
		for (y = 0; y < s->h; y++) {
			int dyy = at->y + y;

			if (dyy < 0 || dyy >= (int)dst->h)
				continue;
			for (x = 0; x < s->w; x++) {
				int dxx = at->x + x;
				uint16_t p = tmp[y * s->w + x];

				if (dxx >= 0 && dxx < (int)dst->w && (p & EMOJI_TAG))
					dst->pixels[dyy * dst->w + dxx] = p & 0x7FFF;
			}
		}
	for (k = 0; live && k < ANIM_RECS; k++)
		if (live & 1u << k) {                        /* frame 0 back, as drawn */
			rec_clear_box(src, &emoji_state.recs[k]);
			rec_put(src, &emoji_state.recs[k], 1);
		}
	return 1;
}

/* The letters of src's window [mask_x + px0, mask_x + px1) displaced for the current step exactly as motion_blit
 * displaces the drawn letters (per-column dx, dy from the span; blink hides), written as ink into omask (the panel
 * W x H outline mask) at panel column px. So a per-character animation moves the OUTLINE with its letters, the way
 * the Studio's renderMessage outlines its already-displaced mask. Columns with no span copy straight through, so a
 * still outline is unchanged. Emoji (tagged) are not outlined. */
void emoji_outline_displace(const struct image *mask, struct image *omask, int mask_x, int px0, int px1, int w, int h)
{
	struct mtable *mt = mask && mask->pixels && mask->format == 1 ? mtable_of(mask->pixels, 0) : 0;
	int a = px0 < 0 ? 0 : px0, b = px1 > w ? w : px1, x, y;
	unsigned k;

	for (x = a; x < b; x++) {
		int sx = mask_x + x, dx = 0, dy = 0, vis = 1;
		const struct mspan *span = 0;

		if (mt)
			for (k = 0; k < mt->n; k++)
				if (sx >= mt->s[k].x0 && sx < mt->s[k].x1) {
					span = &mt->s[k];
					break;
				}
		if (span) {
			int on = span_on(span, emoji_state.t);
			if (span->kind == M_BLINK && span->on)
				vis = on;
			else if (on)
				vis = motion_offset(span->kind, sx - mt->s[0].x0, span->ci, emoji_state.t, &dx, &dy);
		}
		if (!vis)
			continue;
		for (y = 0; y < h; y++) {
			int fx = sx - dx, fy = y - dy;

			if (fx < 0 || fx >= (int)mask->w || fy < 0 || fy >= h)
				continue;
			if (span && (fx < span->x0 || fx >= span->x1))
				continue;
			uint16_t p = mask->pixels[(uint32_t)fy * mask->w + (uint32_t)fx];
			if (p && !(p & EMOJI_TAG))
				omask->pixels[(uint32_t)y * (uint32_t)omask->w + (uint32_t)x] = 0x7FFF;
		}
	}
}

/* glyph_blit_effect (src as a mask, the effect word as fill and background), then every tagged emoji pixel of
 * the source rect drawn in its own colour at the same place. Only message lines carry tagged pixels (emoji_draw);
 * a source without them draws exactly as before. The rect is copied first: the original clips it in place.
 * Animated cells (emoji_draw's records) are taken out of the mask for the blit and drawn at their current frame. */
void ext_glyph_blit_effect(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos)
{
	struct rect s = *r;
	struct point at = *pos;
	uint32_t live = 0;
	unsigned k;
	int x, y;

	if (motion_blit(src, dst, effect, &s, &at))
		return;
	if (emoji_state.recs && src->format == 1 && src->pixels && src->w == TEXT_IMG_W)
		for (k = 0; k < ANIM_RECS; k++)
			if (emoji_state.recs[k].img == src->pixels && rec_live(src, &emoji_state.recs[k])) {
				live |= 1u << k;
				rec_put(src, &emoji_state.recs[k], 0);
			}
	glyph_blit_effect__orig(src, dst, effect, r, pos);
	if (src->format != 1 || !src->pixels || !dst->pixels)
		return;
	for (y = 0; y < s.h; y++) {
		int sy = s.y + y, dy = at.y + y;

		if (sy < 0 || sy >= (int)src->h || dy < 0 || dy >= (int)dst->h)
			continue;
		for (x = 0; x < s.w; x++) {
			int sx = s.x + x, dx = at.x + x;
			uint16_t p;

			if (sx < 0 || sx >= (int)src->w || dx < 0 || dx >= (int)dst->w)
				continue;
			p = src->pixels[sy * src->w + sx];
			if (p & EMOJI_TAG)
				dst->pixels[dy * dst->w + dx] = p & 0x7FFF;
		}
	}
	for (k = 0; live && k < ANIM_RECS; k++)
		if (live & 1u << k) {
			rec_put(src, &emoji_state.recs[k], 1);
			rec_frame(dst, &emoji_state.recs[k], &s, &at);
		}
}
