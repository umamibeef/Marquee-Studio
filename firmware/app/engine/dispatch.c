/* The public names the engine and firmware/ext call, and the weak ext_* defaults. See engine.h. */
#include "engine.h"

#define WEAK __attribute__((weak))

WEAK int ext_text_width(struct font *f, struct sbuf *t) { return text_width__orig(f, t); }
WEAK void ext_text_draw_proportional(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                     uint16_t *colours, uint16_t *width_out)
{
	text_draw_proportional__orig(dst, pos, f, t, colours, width_out);
}
WEAK void ext_text_draw_proportional_plain(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                           int16_t *width_out)
{
	text_draw_proportional_plain__orig(dst, pos, f, t, width_out);
}
WEAK void ext_text_draw_proportional_clip_maybe(struct image *dst, struct point *pos, struct point *fg_origin,
                                                struct point *bg_origin, uint32_t unused, struct font *f,
                                                struct sbuf *t, const uint32_t *effects, uint32_t n_effects,
                                                uint32_t unused2, int16_t *width_out)
{
	text_draw_proportional_clip_maybe__orig(dst, pos, fg_origin, bg_origin, unused, f, t, effects, n_effects, unused2,
	                                        width_out);
}
WEAK void ext_glyph_blit_effect(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos)
{
	glyph_blit_effect__orig(src, dst, effect, r, pos);
}
WEAK void ext_image_blit(struct image *src, struct image *dst, struct rect *r, struct point *pos)
{
	image_blit__orig(src, dst, r, pos);
}
WEAK void ext_textline_init(void *tl, rect_t *box, uint8_t line, int alloc, int mode)
{
	textline_init__orig(tl, box, line, alloc, mode);
}
WEAK int32_t ext_textline_step(void *tl, uint32_t effect) { return textline_step__orig(tl, effect); }
WEAK void emoji_anim_time(uint32_t t) { (void)t; }
WEAK void fonts_load__orig(void) {}

int text_width(struct font *f, struct sbuf *t) { return ext_text_width(f, t); }
void text_draw_proportional(struct image *dst, struct point *pos, struct font *f, struct sbuf *t, uint16_t *colours,
                            uint16_t *width_out)
{
	ext_text_draw_proportional(dst, pos, f, t, colours, width_out);
}
void text_draw_proportional_plain(struct image *dst, struct point *pos, struct font *f, struct sbuf *t, int16_t *width_out)
{
	ext_text_draw_proportional_plain(dst, pos, f, t, width_out);
}
void text_draw_proportional_clip_maybe(struct image *dst, struct point *pos, struct point *fg_origin,
                                       struct point *bg_origin, uint32_t unused, struct font *f, struct sbuf *t,
                                       const uint32_t *effects, uint32_t n_effects, uint32_t unused2, int16_t *width_out)
{
	ext_text_draw_proportional_clip_maybe(dst, pos, fg_origin, bg_origin, unused, f, t, effects, n_effects, unused2,
	                                      width_out);
}
void glyph_blit_effect(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos)
{
	ext_glyph_blit_effect(src, dst, effect, r, pos);
}
void image_blit(struct image *src, struct image *dst, struct rect *r, struct point *pos)
{
	ext_image_blit(src, dst, r, pos);
}
void textline_init(void *tl, rect_t *box, uint8_t line, int alloc, int mode)
{
	ext_textline_init(tl, box, line, alloc, mode);
}
int32_t textline_step(void *tl, uint32_t effect) { return ext_textline_step(tl, effect); }
