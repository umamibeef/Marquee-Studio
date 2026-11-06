/* A message line: textline.ts. Each segment is drawn once into a 0x1414-wide text image, then slid over the line image.
 * Markup (ESC, 0x1B, then):
 *   0x30 hi lo   foreground, big-endian RGB555       0x31 hi lo   background
 *   0x40+d s     entry direction d and speed s       0x44 s       entry 3, from the very left
 *   0x50+e h s   hold h * 4 frames, exit e, speed s  0x54 h s     exit 3, as 0x44
 *   0x60 n       font n; 0xFF proportional (centred) 0x80 n       include line n
 * CR ends a segment. Direction: 0 left, 1 right, 2 up, 3 down. Any other command byte is skipped. */
#include "engine.h"
#include "rt.h"

#define TIW ((int32_t)ENGINE_TEXT_IMG_W)
#define ESC 0x1B
#define CR 0x0D

const uint8_t *message_table;
uint32_t message_table_size;

_Static_assert(__builtin_offsetof(struct textline, state) == 0 && __builtin_offsetof(struct textline, line) == 4,
               "apps.c ext_textline_step reads the line at +4");

static inline uint32_t tb(uint32_t p) { return message_table && p < message_table_size ? message_table[p] : 0; }

/* floor(sqrt(v)), digit by digit: constant shifts only, so no libgcc helper */
static uint32_t isqrt64(uint64_t v)
{
	uint64_t bit = (uint64_t)1 << 62, res = 0;

	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= res + bit) {
			v -= res + bit;
			res = (res >> 1) + bit;
		} else
			res >>= 1;
		bit >>= 2;
	}
	return (uint32_t)res;
}

/* an int up to 2^31 as a float rounds it (round to nearest, ties to even): __floatsisf */
static uint32_t float_round(uint32_t d)
{
	uint32_t shift = 0;

	while ((d >> shift) >= (1u << 24))
		shift++;
	if (!shift)
		return d;
	uint32_t half = 1u << (shift - 1), low = d & ((1u << shift) - 1), hi = d >> shift;
	if (low > half || (low == half && (hi & 1)))
		hi++;
	return hi << shift;      /* (hi may reach 2^24: still exact; d <= 2^31 so this cannot overflow 32 bits) */
}

/* image_slide_init: frames = trunc(sqrt((float)(dx*dx + dy*dy)) / speed * 40.0), in the firmware's doubles. Exactly
 * floor(40 * sqrt(D) / speed) in integers: when D is not a square the true value is too far from an integer for double
 * rounding to cross it, and when it is, (k / 40.0) * 40.0 == k for every integer k (40 = 2^5 + 2^3, Kahan). */
static void slide_init(struct slide *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t speed)
{
	int32_t dx = eng_s16(eng_s16(x1) - eng_s16(x0)), dy = eng_s16(eng_s16(y1) - eng_s16(y0));
	int32_t n;

	s->fx = x0, s->fy = y0, s->tx = x1, s->ty = y1, s->step = 0;
	if (speed) {
		uint32_t d = float_round((uint32_t)(dx * dx) + (uint32_t)(dy * dy));
		uint64_t d64 = d;

		s->frames = isqrt64((d64 << 10) + (d64 << 9) + (d64 << 6)) / speed;   /* 1600 * d */
	} else
		s->frames = 0xFFFFFFFFu;
	n = eng_s16((int32_t)s->frames);
	s->dx100 = n ? eng_s16(dx * 100 / n) : 0;
	s->dy100 = n ? eng_s16(dy * 100 / n) : 0;
}

static void slide_pos(const struct slide *s, struct point *at)
{
	int32_t st = eng_s16((int32_t)s->step);

	at->x = (st * s->dx100 + 50) / 100 + s->fx;
	at->y = (st * s->dy100 + 50) / 100 + s->fy;
}

/* One frame of a slide. 1 when done (that frame blits at the target without clearing first). */
static int slide_run(struct slide *s, struct image *src, struct image *dst)
{
	struct rect whole = {0, 0, (int32_t)src->w, (int32_t)src->h};
	struct point at;

	if (s->frames < s->step) {
		at.x = s->tx, at.y = s->ty;
		image_blit(src, dst, &whole, &at);
		return 1;
	}
	slide_pos(s, &at);
	memset(dst->pixels, 0, dst->w * dst->h * 2);
	image_blit(src, dst, &whole, &at);
	s->step++;
	return 0;
}

void textline_free(struct textline *tl)
{
	free(tl->line_img.pixels);
	free(tl->text_img.pixels);
	tl->line_img.pixels = 0, tl->text_img.pixels = 0;
}

void textline_init__orig(void *tlv, rect_t *box, uint8_t line, int alloc, int mode)
{
	struct textline *tl = tlv;
	struct image li, ti;
	uint32_t W = box->w, H = box->h;

	if (!alloc) {
		li = tl->line_img, ti = tl->text_img;
	} else {
		li.format = ti.format = ENGINE_FORMAT_RGB555;
		li.palette = ti.palette = 0;
		li.w = W, li.h = H, ti.w = ENGINE_TEXT_IMG_W, ti.h = H;
		li.pixels = malloc(W * H * 2);
		ti.pixels = malloc(ENGINE_TEXT_IMG_W * H * 2);
	}
	memset(tl, 0, sizeof *tl);
	tl->line_img = li, tl->text_img = ti;
	tl->line = line;
	tl->W = li.w, tl->H = li.h;
	tl->entry_speed = 40, tl->exit_speed = 40;
	tl->colours[0] = 0x7C00;
	tl->box_x = (int32_t)box->x;
	if (!li.pixels || !ti.pixels) {
		tl->state = 4;
		return;
	}
	memset(li.pixels, 0, li.w * li.h * 2);
	memset(ti.pixels, 0, ti.w * ti.h * 2);
	struct rect b = {(int32_t)box->x, (int32_t)box->y, (int32_t)W, (int32_t)H}, r, r2;
	struct point p;
	font_pick_largest(&tl->f, &b, &r, &r2, &p, mode);
}

static void layout_and_slide(struct textline *tl, int32_t r24)
{
	struct image *ti = &tl->text_img;
	int32_t th = (int32_t)tl->H, lw = (int32_t)tl->W, lh = (int32_t)tl->H;
	struct sbuf text = {tl->text, (uint16_t)tl->text_len, (uint16_t)tl->text_len};

	memset(ti->pixels, 0, ti->w * ti->h * 2);
	if (!tl->proportional) {
		int32_t ch = (int32_t)font_cell_h(&tl->f);

		tl->pos_x = tl->box_x;
		tl->pos_y = (int32_t)((uint32_t)(th - ch) >> 1);
		tl->rect_w = (int32_t)tl->text_len * (int32_t)(tl->f.img.w >> 4);
		tl->rect_h = ch;
	} else {                                                 /* pick a font, then text_centre_in_rect */
		struct rect box = {0, 0, tl->entry_dir < 2 ? TIW : lw, tl->entry_dir < 2 ? th : lh}, r, r2;
		struct point p;
		struct sbuf first = {tl->text, 1, 1};

		font_pick_for_text(&first, &tl->f, &box, &r, &r2, &p, 1, 0);
		int32_t ch = (int32_t)font_cell_h(&tl->f), w = text_width(&tl->f, &text), h = ch < th ? ch : th;
		w = w < TIW ? w : TIW;
		tl->rect_w = w, tl->rect_h = h;
		tl->pos_x = (TIW - w) / 2;
		tl->pos_y = (th - h) / 2;
	}
	struct point at = {tl->pos_x, tl->pos_y};
	uint16_t wo = 0;
	text_draw_proportional(ti, &at, &tl->f, &text, tl->colours, &wo);
	tl->rect_w = wo;
	int32_t px = eng_s16(tl->pos_x), py = eng_s16(tl->pos_y), tw = eng_s16(tl->rect_w), trh = eng_s16(tl->rect_h);
	int32_t cy = eng_s16((int32_t)((uint32_t)(th - lh) >> 1)), cx = eng_s16((int32_t)((uint32_t)(TIW - lw) >> 1));
	uint32_t hold = tl->hold, speed = tl->entry_speed;

	switch (tl->entry_dir) {
	case 0:
		slide_init(&tl->sl, eng_s16(lw - px), -cy, hold == 0 ? -eng_s16(px + tw) : -cx, -cy, speed);
		break;
	case 1:
		slide_init(&tl->sl, -eng_s16(px + tw), -cy, hold == 0 ? eng_s16(lw - px) : -cx, -cy, speed);
		break;
	case 2:
		slide_init(&tl->sl, -cx, eng_s16(lh - py), -cx, hold == 0 ? -eng_s16(py + trh) : -cy, speed);
		break;
	default:
		if (!r24)
			slide_init(&tl->sl, -cx, -eng_s16(lh), -cx, hold == 0 ? eng_s16(lh - py) : -cy, speed);
		else
			slide_init(&tl->sl, 0, -eng_s16(lh), 0, hold == 0 ? eng_s16(lh - py) : -cy, speed);
		break;
	}
}

/* message_text_next_token */
static void next_token(struct textline *tl)
{
	int32_t r24 = tl->r24;
	uint32_t p = tl->line * 256u + tl->lpos[tl->line];

	tl->state = 0;
	for (;;) {
		uint32_t b = tb(p);
		p++;
		tl->lpos[tl->line] = (tl->lpos[tl->line] + 1) & 0xFFFF;
		if (b == 0) {                                        /* end of line: pop an include, or done */
			tl->lpos[tl->line] = 0;
			tl->depth--;
			if (tl->depth & 0x80) {
				tl->state = 4;
				return;
			}
			tl->line = tl->stack[tl->depth];
			p = tl->line * 256u + tl->lpos[tl->line];
			continue;
		}
		if (b != ESC) {
			uint32_t n = 1, c;
			tl->text[0] = (uint8_t)b;
			while ((c = tb(p)) != CR && c != ESC && c != 0 && n < 256) {
				tl->text[n++] = (uint8_t)c;
				p++;
			}
			tl->text_len = n;
			tl->lpos[tl->line] = (p - tl->line * 256u) & 0xFFFF;
			layout_and_slide(tl, r24);
			tl->state = 1;
			return;
		}
		uint32_t cmd = tb(p), a = p + 1;
		tl->lpos[tl->line] = (tl->lpos[tl->line] + 1) & 0xFFFF;
		if (cmd == 0x30 || cmd == 0x31) {                    /* colours, big-endian RGB555 */
			tl->colours[cmd == 0x30 ? 0 : 1] = (uint16_t)(tb(a) << 8 | tb(a + 1));
			p += 3;
			tl->lpos[tl->line] += 2;
		} else if (cmd >= 0x40 && cmd <= 0x44) {             /* entry direction + speed */
			tl->entry_dir = (uint8_t)(cmd - 0x40 < 3 ? cmd - 0x40 : 3);
			tl->entry_speed = (uint8_t)tb(a);
			if (cmd == 0x44)
				r24 = 1;
			p += 2;
			tl->lpos[tl->line] += 1;
		} else if (cmd >= 0x50 && cmd <= 0x54) {             /* hold, exit direction + speed */
			tl->exit_dir = (uint8_t)(cmd - 0x50 < 3 ? cmd - 0x50 : 3);
			tl->hold = tb(a) << 2;
			tl->exit_speed = (uint8_t)tb(a + 1);
			if (cmd == 0x54)
				r24 = 1;
			p += 3;
			tl->lpos[tl->line] += 2;
		} else if (cmd == 0x60) {                            /* font n, or 0xFF: proportional (centred) */
			uint32_t n = tb(a);
			if (n == 0xFF)
				tl->proportional = 1;
			else if (n < font_count) {
				tl->f = font_table[n].f;
				tl->proportional = 0;
			}
			p += 2;
			tl->lpos[tl->line] += 1;
		} else if (cmd == 0x80) {                            /* include line n */
			tl->stack[tl->depth] = tl->line;
			tl->depth++;
			tl->lpos[tl->line] += 1;
			tl->line = (uint8_t)tb(a);
			p = tl->line * 256u + tl->lpos[tl->line];
		} else
			p += 1;                                          /* unknown: skip ESC + command byte */
	}
}

int32_t textline_step__orig(void *tlv, uint32_t effect)
{
	struct textline *tl = tlv;

	(void)effect;
	switch (tl->state) {
	case 0:
		next_token(tl);
		break;
	case 1:
		if (slide_run(&tl->sl, &tl->text_img, &tl->line_img))
			tl->state = tl->hold ? 3 : 0;
		break;
	case 2:
		if (slide_run(&tl->sl, &tl->text_img, &tl->line_img))
			tl->state = 0;
		break;
	case 3: {
		uint32_t d = tl->hold;

		tl->hold = d - 1;
		if (d == 0) {
			int32_t tx = eng_s16(tl->sl.tx), ty = eng_s16(tl->sl.ty), px = eng_s16(tl->pos_x), py = eng_s16(tl->pos_y);
			int32_t tw = eng_s16(tl->rect_w), trh = eng_s16(tl->rect_h);
			uint32_t speed = tl->exit_speed;

			if (tl->exit_dir == 0)
				slide_init(&tl->sl, tx, ty, -eng_s16(px + tw), ty, speed);
			else if (tl->exit_dir == 1)
				slide_init(&tl->sl, tx, ty, eng_s16((int32_t)tl->W - px), ty, speed);
			else if (tl->exit_dir == 2)
				slide_init(&tl->sl, tx, ty, tx, -eng_s16(py + trh), speed);
			else
				slide_init(&tl->sl, tx, ty, tx, eng_s16((int32_t)tl->H - py), speed);
			tl->state = 2;
		}
		break;
	}
	default:
		break;
	}
	return (int32_t)tl->state;
}
