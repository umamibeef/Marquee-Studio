/* The attract loop: attract.ts. The enabled lines with text, one after another, each played once by the text line;
 * each frame the line image goes onto the panel through the line's effect word and outline; one black frame between
 * two lines (with no attract animations, what display_main_loop's attract_play_next_maybe shows there). */
#include "engine.h"
#include "rt.h"

void attract_compose(struct image *frame, struct image *lineimg, struct font *f, const struct attract_line *l,
                     struct image *blank)
{
	int32_t W = (int32_t)frame->w, H = (int32_t)frame->h, cell = (int32_t)font_cell_h(f);
	int32_t y = (int32_t)((uint32_t)(H - cell) >> 1);
	uint32_t word = l->effect;
	struct rect band = {0, y, W, cell};
	struct point at = {0, y};

	if (l->outline_style) {
		struct rect all = {0, 0, W, H}, clip = {0, 0, W, H}, sr = band;
		struct point o = {0, 0};
		int32_t ol[2] = {l->outline_style, (int32_t)((uint32_t)l->outline_colour << 16)};

		glyph_blit_effect(blank, frame, word, &all, &o);     /* the background over the whole panel */
		word = (word & 0xFFFF0000u) | 0x8000u;               /* the letters then leave it alone */
		glyph_outline_blit(lineimg, &sr, frame, &at, ol, &clip);
	}
	glyph_blit_effect(lineimg, frame, word, &band, &at);
}

void attract_init(struct attract *a, const struct attract_line *lines, uint32_t n, uint32_t W, uint32_t H)
{
	uint32_t i, k;

	memset(a, 0, sizeof *a);
	a->W = W, a->H = H, a->cur = -1;
	for (i = 0; i < ATTRACT_LINES && i < n; i++) {
		a->lines[i] = lines[i];
		if (!lines[i].data)
			continue;
		for (k = 0; k < lines[i].len && k < 255; k++)
			a->table[256 * i + k] = lines[i].data[k];
	}
	message_table = a->table;
	message_table_size = sizeof a->table;
	a->blank.format = ENGINE_FORMAT_RGB555, a->blank.w = W, a->blank.h = H;
	a->blank.pixels = malloc(W * H * 2);
	if (a->blank.pixels)
		memset(a->blank.pixels, 0, W * H * 2);
}

void attract_free(struct attract *a)
{
	if (a->allocated)
		textline_free(&a->tl);
	free(a->blank.pixels);
	a->blank.pixels = 0;
	a->allocated = 0, a->have_tl = 0;
}

static int attract_next(struct attract *a)
{
	int32_t k;

	for (k = 1; k <= 16; k++) {
		int32_t i = (a->cur + k + 16) % 16;
		const struct attract_line *l = &a->lines[i];

		if (l->data && l->len && l->data[0]) {
			rect_t box = {(uint32_t)l->box_x, 0, a->W, a->H};

			a->cur = i;
			textline_init(&a->tl, &box, (uint8_t)i, !a->allocated, l->outline_style);
			a->allocated = 1;
			a->have_tl = 1;
			a->line_t = 0;
			return 1;
		}
	}
	a->cur = -1;
	a->have_tl = 0;
	return 0;
}

void attract_step(struct attract *a, struct image *frame)
{
	if (a->gap || (!a->have_tl && !attract_next(a))) {
		a->gap = 0;
		memset(frame->pixels, 0, frame->w * frame->h * 2);
		effect_frame_count++;
		return;
	}
	const struct attract_line *l = &a->lines[a->cur];

	emoji_anim_time(a->line_t++);
	int32_t st = textline_step(&a->tl, l->effect);
	if (a->blank.pixels)
		attract_compose(frame, &a->tl.line_img, &a->tl.f, l, &a->blank);
	effect_frame_count++;
	if (st == 4) {
		a->gap = 1;
		attract_next(a);
	}
}
