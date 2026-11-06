/* sign.h: what an app can do on the sign (docs/APPS.md). An app is restricted C compiled to eBPF:
 *
 *     /opt/homebrew/opt/llvm/bin/clang -target bpfel -O2 -I apps/include -c app.c -o app.o   (tools/mkapp.py does this)
 *
 * It defines two functions:
 *     int init(void)          once when it is loaded (and when its settings change); non-zero = failed
 *     int frame(unsigned t)   each 40 fps frame while it is on screen, t = frames since its turn began;
 *                             return 0 to go on, 1 when its turn is over
 * and draws its picture through the helpers below, into a buffer as big as the panel. The same bytecode runs on the
 * sign (firmware/ext/apps.c) and in the web Studio's preview (frontend/packages/sign-emu src/apps.ts).
 *
 * No C library: no malloc, no stdio, no floating point. Globals are fine (they keep their values between frames).
 * Strings passed to helpers are NUL-terminated and at most 255 bytes. Every helper takes at most 5 arguments. */
#ifndef SIGN_H
#define SIGN_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef long long i64;

/* ---- paint: an RGB555 colour, or one of the sign's effect fills ---- */
#define RGB(r, g, b)  ((((r) >> 3) << 10) | (((g) >> 3) << 5) | ((b) >> 3))   /* 8-bit components */
#define RED     0x7C00
#define GREEN   0x03E0
#define YELLOW  0x7FE0
#define AMBER   0x7E80
#define ORANGE  0x7D00
#define GOLD    0x7EA0
#define WHITE   0x7FFF
#define BLUE    0x001F
#define CYAN    0x03FF
#define MAGENTA 0x7C1F
#define BLACK   0x0000
#define PAINT_WIPE     0x8001             /* red / green / yellow wipe */
#define PAINT_STRIPES  0x8002             /* diagonal red / green / yellow stripes */
#define PAINT_SLOT(n)  (0xC000 | (n))     /* the sign's effect animation n (effect00.flc ..) as a fill */

/* outline styles for outline() */
#define OUTLINE_THICK  1
#define OUTLINE_THIN   2
#define OUTLINE_SHADOW 3
/* outline colours (the sign's 8) */
enum { O_BLACK, O_RED, O_GREEN, O_YELLOW, O_BLUE, O_CYAN, O_MAGENTA, O_WHITE };

/* ---- helpers (numbers are the ABI: never renumber) ---- */
#define HELPER(n, ret, name, ...) static ret (*const name)(__VA_ARGS__) = (void *)(n)
HELPER(1, i32, width, void);                           /* the panel's size */
HELPER(2, i32, height, void);
HELPER(3, void, clear, u32 paint);
HELPER(4, void, rect, i32 x, i32 y, i32 w, i32 h, u32 paint);
HELPER(5, void, pixel, i32 x, i32 y, u32 rgb);
HELPER(6, i32, text, i32 font, i32 x, i32 y, u32 paint, const char *s);   /* returns the width drawn */
HELPER(7, i32, text_width, i32 font, const char *s);
HELPER(8, i32, font_height, i32 font);                 /* -1: no such font */
HELPER(9, i32, font_find, const char *name);           /* "spleen-8x16.mqf", "fonte04.png": -1 if not on the sign */
HELPER(10, void, image, i32 x, i32 y, i32 asset, i32 frame);   /* a bundled picture (or a frame of an animation) */
HELPER(11, i32, asset_find, const char *name);         /* a bundled asset by name, -1 if none */
HELPER(12, i32, asset_info, i32 asset, i32 what);      /* 0 width, 1 height, 2 frames, 3 ms per frame */
HELPER(13, i64, now, void);                            /* the sign's wall-clock time in seconds, -1 until set */
HELPER(14, i32, fmt_time, char *buf, i32 len, const char *fmt, i64 t);   /* strftime: returns the length */
HELPER(15, i32, setting_int, const char *key, i32 dflt);
HELPER(16, i32, setting_str, const char *key, char *buf, i32 len);       /* the length, -1 if unset */
HELPER(17, i32, data_int, const char *key, i32 dflt);
HELPER(18, i32, data_str, const char *key, char *buf, i32 len);          /* the length, -1 if never sent */
HELPER(19, i32, data_age, const char *key);            /* seconds since it was sent, -1 if never */
HELPER(20, void, outline, i32 style, i32 colour);      /* outline everything text() drew this frame */
HELPER(21, void, print, const char *s);                  /* a line on the DB9 console / the Studio's log */
HELPER(22, i32, font_auto, i32 h);                       /* the font the sign picks by itself for an h-high line
                                                            (firmware/auto_fonts.txt), -1 if none */

/* ---- small utilities, compiled into the app ---- */
static inline i32 str_len(const char *s) { i32 n = 0; while (s[n]) n++; return n; }

static inline char *str_copy(char *d, const char *s, i32 cap)
{
	i32 i = 0;
	for (; i < cap - 1 && s[i]; i++)
		d[i] = s[i];
	d[i] = 0;
	return d + i;
}

/* the decimal form of v into b (at least 12 bytes); returns the end */
static inline char *str_int(char *b, i32 v)
{
	char t[12];
	i32 i = 0;
	u32 u = v < 0 ? (u32)(-(v + 1)) + 1u : (u32)v;
	do {
		t[i++] = (char)('0' + u % 10);
		u /= 10;
	} while (u && i < 11);
	if (v < 0)
		*b++ = '-';
	while (i)
		*b++ = t[--i];
	*b = 0;
	return b;
}

/* the panel's centre for something w wide */
static inline i32 centre_x(i32 w) { return (width() - w) / 2; }

#endif
