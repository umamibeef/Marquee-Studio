/* show.h: the usual way an app shows a line of text, driven by its settings (the Studio's Display, Letters,
 * Background and Font controls write them):
 *     font        font name ("" = the sign picks the largest that fits)
 *     colour      letter paint (an RGB555 colour or an effect code, sign.h)    background  background paint
 *     outline     0 none, 1 thick, 2 thin, 3 drop shadow                         outline_colour  0..7
 *     mode        0 held (enter from the right, stop centred, hold, leave left), 1 scrolling (cross once)
 *     background_anim  (the Studio bundles this animation as the asset "background": it plays behind the text)
 *     seconds     how long a held line stays (tenths: 50 = 5 s)                  speed  pixels per second
 * Call show_init() from init() and return show_frame(text, t) from frame(). */
#ifndef SHOW_H
#define SHOW_H
#include "sign.h"

static i32 show_font, show_colour, show_bg, show_outline, show_ocolour, show_mode, show_tenths, show_speed;
static i32 show_anim, show_anim_w, show_anim_h, show_anim_ms;   /* the bundled "background" animation, -1 if none */

/* the font the sign picks by itself for a line as tall as the panel (firmware/auto_fonts.txt), as an attract line does */
static inline i32 show_pick_font(void)
{
	i32 f = font_auto(height());
	return f < 0 ? 0 : f;
}

static inline void show_init(void)
{
	char name[64];
	show_font = setting_str("font", name, sizeof name) > 0 ? font_find(name) : -1;
	if (show_font < 0)
		show_font = show_pick_font();
	show_colour = setting_int("colour", AMBER);
	show_bg = setting_int("background", BLACK);
	show_outline = setting_int("outline", 0);
	show_ocolour = setting_int("outline_colour", O_RED);
	show_mode = setting_int("mode", 0);
	show_tenths = setting_int("seconds", 50);
	show_speed = setting_int("speed", 255);
	if (show_speed < 1)
		show_speed = 1;
	show_anim = asset_find("background");
	if (show_anim >= 0) {
		show_anim_w = asset_info(show_anim, 0);
		show_anim_h = asset_info(show_anim, 1);
		show_anim_ms = asset_info(show_anim, 3);
		if (show_anim_ms < 1)
			show_anim_ms = 100;
	}
}

/* the background animation, tiled across the panel and centred, at its own frame rate (frame t of the turn) */
static inline void show_background(unsigned t)
{
	if (show_anim < 0 || show_anim_w < 1)
		return;
	i32 f = (i32)(t * 25u / (unsigned)show_anim_ms), y = (height() - show_anim_h) / 2;
	for (i32 x = 0; x < width(); x += show_anim_w)
		image(x, y, show_anim, f);
}

/* draw text s for frame t of the app's turn; 1 when the turn is over */
static inline i32 show_frame(const char *s, unsigned t)
{
	i32 W = width(), w = text_width(show_font, s);
	i32 y = (height() - font_height(show_font)) / 2;
	i32 travel = (i32)(t * (unsigned)show_speed / 40u);   /* pixels moved so far */
	i32 x, done = 0;
	if (show_mode == 1) {                                  /* scrolling: from the right edge until gone left */
		x = W - travel;
		done = x + w < 0;
	} else {                                               /* held: in from the right, hold centred, out left */
		i32 home = (W - w) / 2, in = W - home;             /* distance to the centre */
		i32 hold = show_tenths * 4;                        /* frames */
		i32 t_in = (in * 40 + show_speed - 1) / show_speed;
		if ((i32)t < t_in)
			x = W - travel;
		else if ((i32)t < t_in + hold)
			x = home;
		else {
			x = home - (i32)(((unsigned)t - (unsigned)(t_in + hold)) * (unsigned)show_speed / 40u);
			done = x + w < 0;
		}
	}
	clear(show_bg);
	show_background(t);
	text(show_font, x, y, show_colour, s);
	if (show_outline)
		outline(show_outline, show_ocolour);
	return done;
}

/* template with {key} replaced by the pushed value of key ("--" when it was never sent or is older than max_age
 * seconds; max_age 0 = any age) into out */
static inline void expand(const char *tpl, char *out, i32 cap, i32 max_age)
{
	i32 o = 0;
	for (i32 i = 0; tpl[i] && o < cap - 1; i++) {
		if (tpl[i] == '{') {
			char key[24];
			i32 k = 0, j = i + 1;
			while (tpl[j] && tpl[j] != '}' && k < 23)
				key[k++] = tpl[j++];
			key[k] = 0;
			if (tpl[j] == '}') {
				char val[48];
				i32 age = data_age(key), n = data_str(key, val, sizeof val);
				if (n < 0 || (max_age && age > max_age))
					n = 2, val[0] = '-', val[1] = '-', val[2] = 0;
				for (i32 m = 0; m < n && o < cap - 1; m++)
					out[o++] = val[m];
				i = j;
				continue;
			}
		}
		out[o++] = tpl[i];
	}
	out[o] = 0;
}
#endif
