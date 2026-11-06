/* The sign's text engine in freestanding C: fonts, proportional text, effect words and slots, outlines, the message
 * text line, the attract loop, the Compose message frame, strftime. Ported from frontend/packages/sign-emu/src
 * (font.ts, mqf.ts, image.ts, effects.ts, flic.ts, textline.ts, attract.ts, message.ts, strftime.ts).
 *
 * The firmware/ext files declare these structs themselves, so the layouts below are part of the link:
 *   struct image, struct sbuf, struct point, struct rect, rect_t   apps.c, native_emoji.c, fonts.c, update.c, bootscreen.c
 *   struct font (0x618 bytes), struct font_entry (0x718 bytes)     fonts.c, apps.c; update.c and bootscreen.c use the size
 *   struct fonts_state                                             fonts.c
 *   struct textline: state at +0, line at +4                       apps.c ext_textline_step reads tl[4]
 *
 * How the layers fit. Each detoured routine has two names, as in the patched firmware:
 *   name__orig   the base engine, with no native emoji and no in-text (0x1F) commands;
 *   name         what every caller uses, including the engine: it calls ext_name.
 * dispatch.c defines each ext_name as a weak function that calls name__orig. An object that defines a strong ext_name
 * (native_emoji.c, fonts.c, apps.c) replaces it at link time. Link those files as objects, not from an archive: a weak
 * definition already present stops the linker pulling the archive member. */
#ifndef ENGINE_H
#define ENGINE_H
#include <stdint.h>

/* ---- types shared with firmware/ext ---- */
struct image { uint32_t format, w, h; uint16_t *pixels; uint16_t *palette; };
struct font {
	uint32_t layout;                   /* 2: 16 x 16 grid (fonteNN, MQF). 1: 16 x 8. 0: one row. See font_build. */
	struct image img;
	uint8_t left[256], width[256], right[256], proportional[256];
	uint8_t digit_left[256], digit_width[256];   /* layout filler for firmware/ext: the TypeScript has no use for them; zero */
};
struct font_entry { struct font f; char name[256]; };
struct sbuf { const uint8_t *data; uint16_t cap, len; };
struct point { int32_t x, y; };
struct rect { int32_t x, y, w, h; };
typedef struct { uint32_t x, y, w, h; } rect_t;

#define ENGINE_AUTO_MAX 24
struct fonts_state { uint32_t picking, n; uint8_t idx[ENGINE_AUTO_MAX]; };

#define ENGINE_TEXT_IMG_W 0x1414u      /* the text image's width: native_emoji.c tags emoji in images of this width */
#define ENGINE_FORMAT_RGB555 1u

/* ---- font storage ---- */
extern struct font_entry font_table[100];
extern uint8_t font_count;
extern struct fonts_state fonts_state; /* fonts.c fills it. n == 0: picks may choose any font */

/* ---- fonts (font.ts, mqf.ts) ---- */
void font_build(struct font *f, uint32_t layout, struct image *img);   /* img may be &f->img */
uint32_t font_cell_h(const struct font *f);
/* One MQF1 font into fe. hdr: the file's 16-byte header. body: the unpacked body (the caller inflates, so the engine
 * needs no zlib; flags & 1 in the header says it is compressed). Allocates the pixels. 1 on success. */
int font_from_mqf(struct font_entry *fe, const uint8_t *hdr, const uint8_t *body, uint32_t body_len, const char *name);
void font_pick_largest__orig(struct font *out, void *box, void *r, void *r2, void *pos, int mode);
void font_pick_for_text__orig(void *text, struct font *out, void *box, void *r, void *r2, void *pos, int a, int mode);
void font_pick_largest(struct font *out, struct rect *box, struct rect *r, struct rect *r2, struct point *pos, int mode);
void font_pick_for_text(struct sbuf *text, struct font *out, struct rect *box, struct rect *r, struct rect *r2,
                        struct point *pos, int a, int mode);
void fonts_load__orig(void);           /* STUB (weak): does nothing. The firmware has no fonteNN.png loader yet. */

/* ---- pixels and glyph parts (image.ts, font.ts) ---- */
void image_blit__orig(struct image *src, struct image *dst, struct rect *r, struct point *pos);
void image_blit(struct image *src, struct image *dst, struct rect *r, struct point *pos);
/* Both ends inclusive, each pixel clipped. A vertical line with an end outside 16 bits hangs the vendor firmware
 * (font.ts FirmwareHang); here it is only clipped. Non-vertical lines: Bresenham, no TypeScript reference. */
void draw_line(struct image *dst, struct point *from, struct point *to, uint32_t colour);
/* part is relative to the glyph's cell. _cell_part copies the cell's own colours; _char_part: ink fg, the rest bg. */
void font_draw_cell_part(struct image *dst, struct point *pos, struct font *f, uint32_t ch, struct rect *part);
void font_draw_char_part(struct image *dst, struct point *pos, struct font *f, uint32_t ch, struct rect *part,
                         uint32_t fg, uint32_t bg);
void font_draw_char_effect(struct image *dst, struct point *pos, struct point *fg_origin, struct point *bg_origin,
                           struct font *f, uint32_t ch, uint32_t phase, struct rect *part, uint32_t effect);

/* ---- text (font.ts) ---- */
int text_width__orig(struct font *f, struct sbuf *t);
int text_width(struct font *f, struct sbuf *t);
void text_draw_proportional__orig(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                  uint16_t *colours, uint16_t *width_out);
void text_draw_proportional(struct image *dst, struct point *pos, struct font *f, struct sbuf *t, uint16_t *colours,
                            uint16_t *width_out);
void text_draw_proportional_plain__orig(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                        int16_t *width_out);
void text_draw_proportional_plain(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                  int16_t *width_out);
/* effects[i] is the effect word of byte i (the last repeated). Follows native_emoji.c clip_text. */
void text_draw_proportional_clip_maybe__orig(struct image *dst, struct point *pos, struct point *fg_origin,
                                             struct point *bg_origin, uint32_t unused, struct font *f,
                                             struct sbuf *t, const uint32_t *effects, uint32_t n_effects,
                                             uint32_t unused2, int16_t *width_out);
void text_draw_proportional_clip_maybe(struct image *dst, struct point *pos, struct point *fg_origin,
                                       struct point *bg_origin, uint32_t unused, struct font *f, struct sbuf *t,
                                       const uint32_t *effects, uint32_t n_effects, uint32_t unused2,
                                       int16_t *width_out);

/* ---- effects (effects.ts) ---- */
extern uint32_t effect_frame_count;    /* frame_count: textures and slots run on it. One per panel frame. */
/* Back to a known state: frame count fc, every slot stopped, store FLCs and the provider forgotten. */
void effects_reset(uint32_t fc);
void glyph_blit_effect__orig(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos);
void glyph_blit_effect(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos);
/* outline[0]: style (1 thick, 2 thin, 3 drop shadow, anything else nothing); outline[1] >> 16: colour index 0..7. */
void glyph_outline_blit(struct image *src, struct rect *sr, struct image *dst, struct point *pos, int32_t *outline,
                        struct rect *clip);
uint32_t effect_colour_index_to_rgb555(int32_t i);
/* Effect slots (0xC000|n). Order: a store FLC, else the built-in table (effect_slots.h, n < NSLOT), else solid yellow.
 * effect_slot_set_flc: slot n plays this FLC from now on (the bytes must stay valid). data == 0 goes back to the
 * built-in. 1 on success; 0 if n >= 100, the data is not a FLIC, or memory ran out (the slot then stays as it was).
 * effect_slot_provider: asked once per slot, when the slot is first drawn, for store-provided bytes; return 1 and set
 * *data, *size if the store has them. Typically store_find("effectNN.flc", ...). */
typedef int (*effect_flc_provider)(unsigned n, const uint8_t **data, uint32_t *size);
int effect_slot_set_flc(unsigned n, const uint8_t *data, uint32_t size);
void effect_slot_set_provider(effect_flc_provider p);
/* Panel frames in one loop of slot n; 1 for a slot that does not play. */
uint32_t effect_slot_period(unsigned n);

/* ---- FLIC (flic.ts) ---- */
struct flic {
	const uint8_t *d;
	uint32_t size, frames, w, h, depth, speed_ms, noff;
	uint32_t *off;                     /* the frame chunks' file offsets */
	uint16_t *px;                      /* w * h: palette indices, or RGB555 at depth 16 */
	uint16_t *img;                     /* w * h: the current frame as RGB555 */
	uint16_t palette[256 * 3];
};
int flic_open(struct flic *fl, const uint8_t *data, uint32_t size);   /* 1 on success; flic_close frees */
void flic_close(struct flic *fl);
void flic_decode(struct flic *fl, uint32_t off);                       /* apply the frame chunk at off, then fl->img */
uint32_t flc_speed_to_hold(uint32_t speed);

/* ---- the text line (textline.ts) ---- */
/* The message lines, 256 bytes each; a read past message_table_size gives 0. Set before textline_init. */
extern const uint8_t *message_table;
extern uint32_t message_table_size;
struct slide { int32_t fx, fy, tx, ty; uint32_t step, frames; int32_t dx100, dy100; };
struct textline {
	uint32_t state;                    /* +0: 0 next segment, 1 slide in, 2 slide out, 3 hold, 4 done */
	uint8_t line;                      /* +4: read by apps.c */
	uint8_t depth, entry_dir, exit_dir, entry_speed, exit_speed, proportional, pad;
	uint16_t colours[2];               /* fg, bg */
	uint32_t hold;
	int32_t r24, box_x;                /* r24: textline.ts's constructor argument, 0 as textline_init leaves it */
	int32_t pos_x, pos_y, rect_w, rect_h;
	uint32_t W, H;
	struct image line_img, text_img;
	struct slide sl;
	struct font f;
	uint32_t text_len;
	uint32_t lpos[256];
	uint8_t stack[256];
	uint8_t text[256];
};
/* tl: a struct textline. alloc != 0: allocates the line image (box w x h) and text image (0x1414 x h), and tl is
 * written from scratch. alloc == 0: reuses the images of an earlier init and clears them. mode: the outline style,
 * which shrinks the box font_pick_largest fits: 1, 2 by 2 px; 3 by 1 px. On allocation failure the pixels are 0 and
 * textline_step reports done. */
void textline_init__orig(void *tl, rect_t *box, uint8_t line, int alloc, int mode);
void textline_init(void *tl, rect_t *box, uint8_t line, int alloc, int mode);
/* One 40 fps frame into tl's line_img; returns the state. effect: not used, as in textline.ts. */
int32_t textline_step__orig(void *tl, uint32_t effect);
int32_t textline_step(void *tl, uint32_t effect);
void textline_free(struct textline *tl);

/* ---- the attract loop (attract.ts) ---- */
struct attract_line {
	const uint8_t *data;               /* text and markup; 0: the line is off. At most 255 bytes are used. */
	uint32_t len;
	uint32_t effect;                   /* FG << 16 | BG */
	uint8_t outline_style;             /* 0 none, 1 thick, 2 thin, 3 drop shadow */
	uint8_t outline_colour;            /* 0..7 */
	int32_t box_x;                     /* where a fixed-font segment starts in the text image */
};
#define ATTRACT_LINES 16
struct attract {
	uint32_t W, H;
	struct attract_line lines[ATTRACT_LINES];
	uint8_t table[256 * 17];
	struct textline tl;
	int32_t cur;                       /* the line on screen, -1 none */
	uint32_t have_tl, allocated, gap, line_t;
	struct image blank;                /* W x H zeros: the mask that paints the background */
};
/* Copies n (at most 16) lines into a's table and points message_table at it. The heap holds the blank image, and once
 * a line runs its line image and text image (0x1414 x H). */
void attract_init(struct attract *a, const struct attract_line *lines, uint32_t n, uint32_t W, uint32_t H);
/* One 40 fps frame into frame (W x H; the caller's buffer, NOT cleared, as the firmware draws over the oldest of 8):
 * the current line through its effect word and outline, or black for the one frame between two lines and when no line
 * has text. Advances effect_frame_count once. After it, a->cur is the line that comes next. */
void attract_step(struct attract *a, struct image *frame);
/* composeAttract alone: line image lineimg, drawn in font f, onto frame through l's effect and outline.
 * blank: a zero image at least W x H. */
void attract_compose(struct image *frame, struct image *lineimg, struct font *f, const struct attract_line *l,
                     struct image *blank);
void attract_free(struct attract *a);

/* ---- a Compose message (message.ts), without native emoji or in-text effects ---- */
/* drawTextItem: the background over the window, the outline around the mask, then the letters. blank: as above. */
void draw_text_item(struct image *frame, struct image *mask, uint32_t word, struct rect *window, int32_t style,
                    int32_t colour, struct image *blank);
struct message_params {
	const uint8_t *text;               /* font codes */
	uint32_t len;
	struct font *f;
	uint32_t word;                     /* FG << 16 | BG */
	int32_t outline_style, outline_colour;
	int32_t flash_on, flash_off;       /* panel frames */
	uint32_t frame0;                   /* frame_count at frame 0; 1000 is the Studio's default */
	int32_t is_static;                 /* centred; else scrolled */
	int32_t text_rate;                 /* px per frame when scrolled */
	uint32_t W, H;
};
uint32_t message_frame_count(const struct message_params *m);
/* Frame k into frame (black first). mask, blank: W x H; mask is overwritten. Sets effect_frame_count = frame0 + k. */
void message_frame(const struct message_params *m, uint32_t k, struct image *frame, struct image *mask,
                   struct image *blank);
/* amountFormat: a money value as font codes, into out (max bytes); returns the length. dec, thou: 0 for none. */
uint32_t amount_format(uint8_t *out, uint32_t max, uint32_t value, uint32_t decimals, uint32_t mode, uint8_t dec,
                       uint8_t thou, const uint8_t *prefix, uint32_t prefix_len, const uint8_t *suffix,
                       uint32_t suffix_len);

/* ---- strftime (strftime.ts) ---- */
/* Format seconds t since 1970 (wall-clock: no time zone is applied) into out, NUL-terminated; returns the length,
 * at most max - 1. Unknown conversions stay as written. */
uint32_t sign_strftime(char *out, uint32_t max, const char *fmt, int64_t t);

/* ---- the detour points: weak in dispatch.c ---- */
int ext_text_width(struct font *f, struct sbuf *t);
void ext_text_draw_proportional(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                uint16_t *colours, uint16_t *width_out);
void ext_text_draw_proportional_plain(struct image *dst, struct point *pos, struct font *f, struct sbuf *t,
                                      int16_t *width_out);
void ext_text_draw_proportional_clip_maybe(struct image *dst, struct point *pos, struct point *fg_origin,
                                           struct point *bg_origin, uint32_t unused, struct font *f, struct sbuf *t,
                                           const uint32_t *effects, uint32_t n_effects, uint32_t unused2,
                                           int16_t *width_out);
void ext_glyph_blit_effect(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos);
void ext_image_blit(struct image *src, struct image *dst, struct rect *r, struct point *pos);
void ext_font_pick_largest(struct font *out, void *box, void *r, void *r2, void *pos, int mode);
void ext_font_pick_for_text(void *text, struct font *out, void *box, void *r, void *r2, void *pos, int a, int mode);
void ext_textline_init(void *tl, rect_t *box, uint8_t line, int alloc, int mode);
int32_t ext_textline_step(void *tl, uint32_t effect);
void emoji_anim_time(uint32_t t);

#endif
