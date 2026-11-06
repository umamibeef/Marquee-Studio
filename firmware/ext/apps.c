/* Programmable apps on the sign (docs/APPS.md): app bundles (MQAP1, tools/mkapp.py) named app0.mqap .. app15.mqap
 * in the content store (store.c) each own an attract line. When the attract loop reaches the line, the app's eBPF
 * code (ebpf.c) runs once per frame in place of the text tokenizer and draws, through the helpers below, into a
 * buffer as big as the panel; that buffer is the panel's frame. The web Studio runs the same bytecode with the same
 * helpers (frontend/packages/sign-emu src/appvm.ts); the simulator test compares the two frame for frame.
 *
 * The sign's time: the console's "time" command sets it (sign seconds: local wall-clock seconds since 1970), and it
 * counts on with the RTEMS clock. */
#include <stdint.h>
#include "ebpf.h"

/* ---- firmware types and routines (analysis/symbols.tsv) ---- */
struct image { uint32_t format, w, h; uint16_t *pixels; uint16_t *palette; };
struct font {
	uint32_t layout;
	struct image img;
	uint8_t left[256], width[256], right[256], proportional[256], digit_left[256], digit_width[256];
};
struct font_entry { struct font f; char name[256]; };
struct sbuf { const uint8_t *data; uint16_t cap, len; };
struct point { int32_t x, y; };
struct rect { int32_t x, y, w, h; };
struct lfs_file { const uint8_t *data; uint32_t size, f8, flags, f10, pos, f18, f1c, f20, name, index, f2c; };
typedef struct { uint32_t x, y, w, h; } rect_t;

void *malloc(unsigned long size);
void free(void *p);
int lfs_open(struct lfs_file *fh, const char *name);
int uncompress(uint8_t *dst, uint32_t *dst_len, const uint8_t *src, uint32_t src_len);
uint32_t display_width(void);
uint32_t display_height(void);
int text_width(struct font *f, struct sbuf *t);
void text_draw_proportional(struct image *dst, struct point *pos, struct font *f, struct sbuf *t, uint16_t *colours,
                            uint16_t *width_out);
void glyph_blit_effect(struct image *src, struct image *dst, uint32_t effect, struct rect *r, struct point *pos);
void glyph_outline_blit(struct image *src, struct rect *sr, struct image *dst, struct point *pos, int32_t *outline,
                        struct rect *clip);
void emoji_anim_time(uint32_t t);             /* native_emoji.c: the clock of animated emoji */
void textline_init__orig(void *tl, rect_t *box, uint8_t line, int alloc, int mode);
int32_t textline_step__orig(void *tl, uint32_t effect);
void frame_submit__orig(struct image *frame);
void console_log_line(const char *s);                 /* console.c */
int store_find(const char *name, const uint8_t **data, uint32_t *size);   /* store.c */
extern struct font_entry font_table[100];
extern uint8_t font_count;

#include "../app/core.h"
#define TEXT_IMG_W  0x1414                    /* the firmware's text image width: our text mask */
#define MASK_X      2048                      /* x = 0 in the mask */
#define DATA_VA     0x20000000u
#define BUDGET      200000u
#define NLINES      16
#define NDATA       12

/* ---- state (ext_apps_ram: fwext.py) ---- */
struct datum { char key[16]; uint8_t value[48]; uint32_t len, at; };
#define NASSETS 8
struct asset_ { const uint8_t *name; uint32_t nl, w, h, frames, ms; uint8_t *px; };
struct app {
	struct lfs_file fh;                         /* the bundle in the store */
	struct asset_ as[NASSETS];                  /* its assets, unpacked at load */
	uint32_t nassets;
	const uint8_t *code, *settings, *assets;
	uint32_t code_len, settings_len, assets_len, init_pc, frame_pc, mem_len;
	uint8_t *mem;
	uint32_t t, error_shown;
	char error[64];
	struct datum data[NDATA];
	struct ebpf_vm vm;
};
struct apps_state {
	uint32_t magic, valid, base, base_ticks;
	uint32_t line_steps;                        /* steps of the message line on screen (animated emoji) */
	struct app *app[NLINES];
	int32_t active;                             /* the line whose app is on screen, -1 none */
	uint32_t overlay;                           /* this frame shows the active app's buffer */
	uint32_t W, H;
	struct image buf, mask, omask;
	uint32_t store_busy;                        /* store.c is writing main flash: do not open files */
	ebpf_helper helpers[23];                    /* apps/include/sign.h: filled at boot (no relocated tables) */
	void *store;                                /* store.c's staging state */
	void *update;                               /* update.c's state: a firmware update in progress */
	uint32_t runs, overlays, inits;             /* counters for the console's "apps" */
	uint32_t submits, upd_frames;               /* frame_submit calls; those showing the update */
	uint32_t last_lr, last_px, last_wh;         /* the last frame_submit: its caller, pixels, w << 16 | h */
	uint32_t version_shown;                     /* bootscreen.c: the version after the logo, once a start */
};
static struct apps_state g_apps;
#define S g_apps
#define STATE_MAGIC 0x41505053u                 /* "APPS" */

static void helpers_fill(void);

void apps_boot(void)
{
	S.magic = STATE_MAGIC;
	S.active = -1;
	helpers_fill();
}

/* ---- time ---- */
static uint32_t ticks(void)
{
	uint32_t t = 0;
	t = clock_ticks();
	return t;
}

static uint32_t tps(void)
{
	return CLOCK_TPS;
}

void apps_set_time(uint32_t secs)
{
	S.base = secs;
	S.base_ticks = ticks();
	S.valid = 1;
}

uint32_t apps_now(int *valid)
{
	*valid = (int)S.valid;
	return S.base + (ticks() - S.base_ticks) / tps();
}

uint32_t apps_uptime(void) { return ticks() / tps(); }

static int64_t now_s(void) { return S.valid ? (int64_t)(S.base + (ticks() - S.base_ticks) / tps()) : -1; }

/* ---- strftime (frontend/packages/sign-emu src/appvm.ts signStrftime) ---- */
struct out { uint8_t *p; unsigned n, max; };
static void put(struct out *o, char c) { if (o->n < o->max) o->p[o->n++] = (uint8_t)c; }
static void puts_(struct out *o, const char *s) { while (*s) put(o, *s++); }
static void putn(struct out *o, int32_t v, int width, char pad)
{
	char b[12];
	int n = 0;
	uint32_t u = v < 0 ? (uint32_t)(-(v + 1)) + 1u : (uint32_t)v;
	do { b[n++] = (char)('0' + u % 10); u /= 10; } while (u);
	if (v < 0) put(o, '-');
	for (int k = n; k < width; k++) put(o, pad);
	while (n) put(o, b[--n]);
}

static const char DAYS[7][10] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char MONTHS[12][10] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December"};
struct tm_ { int year, mon, mday, hour, min, sec, wday, yday; };

static int64_t fdiv(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q; }

static void breakdown(int64_t t, struct tm_ *tm)
{
	int64_t days = fdiv(t, 86400), rem = t - days * 86400;
	tm->hour = (int)(rem / 3600); tm->min = (int)(rem / 60 % 60); tm->sec = (int)(rem % 60);
	tm->wday = (int)(((days + 4) % 7 + 7) % 7);
	int64_t z = days + 719468, era = fdiv(z, 146097), doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
	tm->mday = (int)(doy - (153 * mp + 2) / 5 + 1);
	tm->mon = (int)(mp < 10 ? mp + 3 : mp - 9) - 1;
	tm->year = (int)(y + (tm->mon < 2));
	static const uint16_t before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	int leap = (tm->year % 4 == 0 && tm->year % 100 != 0) || tm->year % 400 == 0;
	tm->yday = before[tm->mon] + tm->mday - 1 + (leap && tm->mon > 1);
}

static void strftime_(struct out *o, const uint8_t *f, int64_t t)
{
	struct tm_ tm;
	breakdown(t, &tm);
	for (unsigned i = 0; f[i]; i++) {
		if (f[i] != '%' || !f[i + 1]) {
			put(o, (char)f[i]);
			continue;
		}
		char c = (char)f[++i];
		int h12 = tm.hour % 12 ? tm.hour % 12 : 12;
		switch (c) {
		case 'H': putn(o, tm.hour, 2, '0'); break;
		case 'M': putn(o, tm.min, 2, '0'); break;
		case 'S': putn(o, tm.sec, 2, '0'); break;
		case 'I': putn(o, h12, 2, '0'); break;
		case 'l': putn(o, h12, 1, '0'); break;
		case 'p': puts_(o, tm.hour < 12 ? "AM" : "PM"); break;
		case 'P': puts_(o, tm.hour < 12 ? "am" : "pm"); break;
		case 'k': putn(o, tm.hour, 2, ' '); break;
		case 'j': putn(o, tm.yday + 1, 3, '0'); break;
		case 'u': putn(o, tm.wday ? tm.wday : 7, 1, '0'); break;
		case 'w': putn(o, tm.wday, 1, '0'); break;
		case 'F': putn(o, tm.year, 1, '0'); put(o, '-'); putn(o, tm.mon + 1, 2, '0'); put(o, '-'); putn(o, tm.mday, 2, '0'); break;
		case 'D': putn(o, tm.mon + 1, 2, '0'); put(o, '/'); putn(o, tm.mday, 2, '0'); put(o, '/'); putn(o, tm.year % 100, 2, '0'); break;
		case 'a': put(o, DAYS[tm.wday][0]); put(o, DAYS[tm.wday][1]); put(o, DAYS[tm.wday][2]); break;
		case 'A': puts_(o, DAYS[tm.wday]); break;
		case 'b': put(o, MONTHS[tm.mon][0]); put(o, MONTHS[tm.mon][1]); put(o, MONTHS[tm.mon][2]); break;
		case 'B': puts_(o, MONTHS[tm.mon]); break;
		case 'd': putn(o, tm.mday, 2, '0'); break;
		case 'e': putn(o, tm.mday, 2, ' '); break;
		case 'm': putn(o, tm.mon + 1, 2, '0'); break;
		case 'y': putn(o, tm.year % 100, 2, '0'); break;
		case 'Y': putn(o, tm.year, 1, '0'); break;
		case 'T': putn(o, tm.hour, 2, '0'); put(o, ':'); putn(o, tm.min, 2, '0'); put(o, ':'); putn(o, tm.sec, 2, '0'); break;
		case 'R': putn(o, tm.hour, 2, '0'); put(o, ':'); putn(o, tm.min, 2, '0'); break;
		case '%': put(o, '%'); break;
		default: put(o, '%'); put(o, c); break;
		}
	}
}

/* ---- bundles ---- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

static void set_error(struct app *a, const char *what, uint32_t pc)
{
	struct out o = {(uint8_t *)a->error, 0, sizeof a->error - 1};
	puts_(&o, what);
	if (pc != 0xFFFFFFFFu) {
		puts_(&o, " at pc ");
		putn(&o, (int32_t)pc, 1, '0');
	}
	a->error[o.n] = 0;
}

#define NHELPERS 23
static int images(void);

/* the app of line n, loaded from app<n>.mqap (its memory then lives as long as the store does not change) */
static struct app *app_load(unsigned n)
{
	if (S.app[n])
		return S.app[n];
	if (S.store_busy || !images())                          /* the panel's size first: init() may ask for it */
		return 0;
	char name[16] = "app";
	unsigned k = 3;
	if (n >= 10)
		name[k++] = (char)('0' + n / 10);
	name[k++] = (char)('0' + n % 10);
	name[k++] = '.', name[k++] = 'm', name[k++] = 'q', name[k++] = 'a', name[k++] = 'p', name[k] = 0;
	struct lfs_file fh;
	const uint8_t *sd;
	uint32_t ss;
	if (store_find(name, &sd, &ss)) {                       /* the content store (store.c), else the image */
		fh.data = sd;
		fh.size = ss;
	} else if (!lfs_open(&fh, name))
		return 0;
	if (fh.size < 40 || be32(fh.data) != 0x4D514150u)
		return 0;
	struct app *a = malloc(sizeof *a);
	if (!a)
		return 0;
	uint8_t *z = (uint8_t *)a;
	for (unsigned i = 0; i < sizeof *a; i++)
		z[i] = 0;
	a->fh = fh;
	const uint8_t *h = fh.data;
	uint32_t code = be32(h + 8), data = be32(h + 12), bss = be32(h + 16);
	a->init_pc = be32(h + 20), a->frame_pc = be32(h + 24);
	a->settings_len = be32(h + 28), a->assets_len = be32(h + 32);
	a->code = h + 40, a->code_len = code;
	a->settings = a->code + code + data;
	a->assets = a->settings + a->settings_len;
	if (be16(h + 4) != 1 || 40 + code + data + a->settings_len + a->assets_len > fh.size)
		set_error(a, "not a valid app (ABI 1)", 0xFFFFFFFFu);
	else if (ebpf_check(a->code, code / 8, NHELPERS))
		set_error(a, "invalid instruction", ebpf_check(a->code, code / 8, NHELPERS) - 1);
	else if (!(a->mem = malloc(data + bss + 1)))
		set_error(a, "out of memory", 0xFFFFFFFFu);
	else {
		for (uint32_t i = 0; i < data; i++)
			a->mem[i] = a->code[code + i];
		for (uint32_t i = data; i < data + bss; i++)
			a->mem[i] = 0;
		a->mem_len = data + bss;
	}
	if (!a->error[0] && a->assets_len >= 2) {              /* the assets: unpacked once */
		const uint8_t *p = a->assets + 2, *end = a->assets + a->assets_len;
		uint32_t count = be16(a->assets);
		for (uint32_t k = 0; k < count && p + 13 <= end && !a->error[0]; k++) {
			struct asset_ *as = &a->as[a->nassets];
			uint32_t nl = p[0];
			const uint8_t *q = p + 1 + nl;
			uint32_t w = be16(q), h = be16(q + 2), fr = be16(q + 4), ms = be16(q + 6), zl = be32(q + 8);
			uint32_t n = 2 * w * h * fr;
			p = q + 12 + zl;
			if (p > end) {
				set_error(a, "asset past the bundle", 0xFFFFFFFFu);
				break;
			}
			if (a->nassets >= NASSETS)
				continue;
			as->name = p - zl - 12 - nl, as->nl = nl, as->w = w, as->h = h, as->frames = fr, as->ms = ms;
			if (!(as->px = malloc(n ? n : 1)) || uncompress(as->px, &n, q + 12, zl) || n != 2 * w * h * fr)
				set_error(a, "asset unpacking failed", 0xFFFFFFFFu);
			a->nassets++;
		}
	}
	a->vm.code = a->code, a->vm.ninsts = code / 8;
	a->vm.helpers = S.helpers, a->vm.nhelpers = NHELPERS;
	a->vm.user = a;
	ebpf_region(&a->vm, EBPF_STACK_VA, EBPF_STACK, a->vm.stack, 1);
	ebpf_region(&a->vm, DATA_VA, a->mem_len, a->mem, 1);
	S.app[n] = a;
	if (!a->error[0]) {                                     /* init() */
		a->vm.budget = BUDGET;
		uint64_t r = ebpf_run(&a->vm, a->init_pc, 0);
		if (a->vm.error)
			set_error(a, a->vm.error == EBPF_E_BUDGET ? "init: out of time" : "init failed", a->vm.error_pc);
		else if ((uint32_t)r)
			set_error(a, "init() returned non-zero", 0xFFFFFFFFu);
	}
	return a;
}

/* forget every loaded app (the store changed) */
void apps_unload(void)
{
	for (unsigned n = 0; n < NLINES; n++)
		if (S.app[n]) {
			for (uint32_t k = 0; k < S.app[n]->nassets; k++)
				free(S.app[n]->as[k].px);
			free(S.app[n]->mem);
			free(S.app[n]);
			S.app[n] = 0;
		}
	S.active = -1;
}

/* ---- the buffers ---- */
static int images(void)
{
	if (S.buf.pixels)
		return 1;
	S.W = display_width(), S.H = display_height();
	uint32_t W = S.W, H = S.H;
	S.buf.pixels = malloc(W * H * 2), S.omask.pixels = malloc(W * H * 2), S.mask.pixels = malloc(TEXT_IMG_W * H * 2);
	if (!S.buf.pixels || !S.omask.pixels || !S.mask.pixels)
		return 0;
	S.buf.format = S.mask.format = S.omask.format = 1;
	S.buf.w = S.omask.w = W, S.buf.h = S.omask.h = S.mask.h = H, S.mask.w = TEXT_IMG_W;
	for (uint32_t i = 0; i < TEXT_IMG_W * H; i++)
		S.mask.pixels[i] = 0;
	for (uint32_t i = 0; i < W * H; i++)
		S.buf.pixels[i] = 0, S.omask.pixels[i] = 0;
	return 1;
}

/* the mask's window [MASK_X, 0, W, H] onto the buffer with paint; then clear the mask columns [x0, x1) (marking
 * their ink in the outline mask first) */
void emoji_effects_forget(const struct image *img);         /* native_emoji.c */
/* native_emoji.c: the letters of the mask window displaced for this step, as ink in the outline mask (panel cols),
 * so the outline follows a per-letter animation. */
void emoji_outline_displace(const struct image *mask, struct image *omask, int mask_x, int px0, int px1, int w, int h);
static void paint(uint32_t p, int32_t x0, int32_t x1)
{
	struct rect r = {MASK_X, 0, (int32_t)S.W, (int32_t)S.H};
	struct point at = {0, 0};
	glyph_blit_effect(&S.mask, &S.buf, (p & 0xFFFF) << 16 | 0x8000, &r, &at);
	/* Build the outline source from the DISPLACED letters (before the spans are forgotten), the way the Studio
	 * outlines its already-displaced mask: the outline then waves/bounces/shakes/blinks with its letters. Still
	 * text displaces by 0, so it is unchanged. Emoji are not outlined. */
	emoji_outline_displace(&S.mask, &S.omask, MASK_X, x0 - MASK_X, x1 - MASK_X, (int)S.W, (int)S.H);
	emoji_effects_forget(&S.mask);                          /* the text's per-letter effects end with it */
	int32_t a = x0 < 0 ? 0 : x0, b = x1 > TEXT_IMG_W ? TEXT_IMG_W : x1;
	for (uint32_t y = 0; y < S.H; y++)
		for (int32_t x = a; x < b; x++)
			S.mask.pixels[y * TEXT_IMG_W + (uint32_t)x] = 0;
}

static void rect_(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t p)
{
	int32_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int32_t x1 = x + w > (int32_t)S.W ? (int32_t)S.W : x + w, y1 = y + h > (int32_t)S.H ? (int32_t)S.H : y + h;
	if (x1 <= x0 || y1 <= y0)
		return;
	for (int32_t yy = y0; yy < y1; yy++)
		for (int32_t xx = x0; xx < x1; xx++)
			S.mask.pixels[(uint32_t)yy * TEXT_IMG_W + MASK_X + (uint32_t)xx] = 0x7FFF;
	paint(p, MASK_X + x0, MASK_X + x1);
	for (int32_t yy = y0; yy < y1; yy++)
		for (int32_t xx = x0; xx < x1; xx++)
			S.omask.pixels[(uint32_t)yy * S.W + (uint32_t)xx] = 0;
}

/* ---- helpers (apps/include/sign.h) ---- */
#define APP ((struct app *)vm->user)
#define I32(v) ((uint64_t)(int64_t)(int32_t)(v))

/* a NUL-terminated string of at most 255 bytes in the app's memory, or stop the app */
static const uint8_t *str(struct ebpf_vm *vm, uint64_t va, uint32_t *len)
{
	for (uint32_t n = 1; n <= 256; n++) {
		uint8_t *p = ebpf_mem(vm, va, n, 0);
		if (!p)
			break;
		if (!p[n - 1]) {
			*len = n - 1;
			return p;
		}
	}
	ebpf_stop(vm, EBPF_E_MEMORY);
	return 0;
}

static int32_t out(struct ebpf_vm *vm, uint64_t va, int32_t len, const uint8_t *src, uint32_t n)
{
	if (len <= 0)
		return -1;
	uint8_t *p = ebpf_mem(vm, va, (uint32_t)len, 1);
	if (!p) {
		ebpf_stop(vm, EBPF_E_MEMORY);
		return -1;
	}
	if (n > (uint32_t)len - 1)
		n = (uint32_t)len - 1;
	for (uint32_t k = 0; k < n; k++)
		p[k] = src[k];
	p[n] = 0;
	return (int32_t)n;
}

static struct font *font_(uint64_t n) { return (int32_t)n >= 0 && (int32_t)n < font_count ? &font_table[(int32_t)n].f : 0; }

static int keq(const uint8_t *a, uint32_t n, const char *b) { for (uint32_t k = 0; k < n; k++) if (a[k] != (uint8_t)b[k]) return 0; return !b[n]; }

/* the bundle's setting key: its value and length (0 if unset) */
static const uint8_t *setting(struct app *a, const uint8_t *key, uint32_t klen, uint32_t *vlen)
{
	const uint8_t *p = a->settings, *end = p + a->settings_len;
	while (p < end) {
		const uint8_t *e = p;
		while (e < end && *e)
			e++;
		const uint8_t *eq = p;
		while (eq < e && *eq != '=')
			eq++;
		if (eq < e && (uint32_t)(eq - p) == klen) {
			uint32_t k = 0;
			while (k < klen && p[k] == key[k])
				k++;
			if (k == klen) {
				*vlen = (uint32_t)(e - eq - 1);
				return eq + 1;
			}
		}
		p = e + 1;
	}
	return 0;
}

static int32_t parse_int(const uint8_t *v, uint32_t n, int32_t dflt)
{
	if (!v || !n)
		return dflt;
	uint32_t i = 0, neg = 0;
	int32_t r = 0;
	if (v[0] == '-')
		neg = 1, i = 1;
	if (i >= n)
		return dflt;
	for (; i < n; i++) {
		if (v[i] < '0' || v[i] > '9')
			return dflt;
		r = (int32_t)((uint32_t)r * 10u + (uint32_t)(v[i] - '0'));
	}
	return neg ? (int32_t)(0u - (uint32_t)r) : r;
}

static struct datum *datum(struct app *a, const uint8_t *key, uint32_t klen)
{
	for (unsigned k = 0; k < NDATA; k++) {
		const char *dk = a->data[k].key;
		uint32_t n = 0;
		while (n < sizeof a->data[k].key && dk[n])
			n++;
		if (!n || n != klen)
			continue;
		uint32_t i = 0;
		while (i < n && (uint8_t)dk[i] == key[i])
			i++;
		if (i == n)
			return &a->data[k];
	}
	return 0;
}

static const uint8_t *asset(struct app *a, int32_t idx, uint32_t *w, uint32_t *h, uint32_t *frames, uint32_t *ms)
{
	if (idx < 0 || (uint32_t)idx >= a->nassets || !a->as[idx].px)
		return 0;
	struct asset_ *as = &a->as[idx];
	*w = as->w, *h = as->h, *frames = as->frames, *ms = as->ms;
	return as->px;
}

static uint64_t h_width(struct ebpf_vm *vm, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { (void)vm, (void)a, (void)b, (void)c, (void)d, (void)e; return S.W; }
static uint64_t h_height(struct ebpf_vm *vm, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { (void)vm, (void)a, (void)b, (void)c, (void)d, (void)e; return S.H; }
static uint64_t h_clear(struct ebpf_vm *vm, uint64_t p, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { (void)vm, (void)b, (void)c, (void)d, (void)e; rect_(0, 0, (int32_t)S.W, (int32_t)S.H, (uint32_t)p); return 0; }
static uint64_t h_rect(struct ebpf_vm *vm, uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint64_t p) { (void)vm; rect_((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (uint32_t)p); return 0; }
static uint64_t h_pixel(struct ebpf_vm *vm, uint64_t x, uint64_t y, uint64_t c, uint64_t d, uint64_t e)
{
	(void)vm, (void)d, (void)e;
	int32_t X = (int32_t)x, Y = (int32_t)y;
	if (X >= 0 && X < (int32_t)S.W && Y >= 0 && Y < (int32_t)S.H)
		S.buf.pixels[(uint32_t)Y * S.W + (uint32_t)X] = (uint16_t)(c & 0x7FFF);
	return 0;
}
static uint64_t h_text(struct ebpf_vm *vm, uint64_t f, uint64_t x, uint64_t y, uint64_t p, uint64_t s)
{
	struct font *ft = font_(f);
	uint32_t n;
	const uint8_t *t = str(vm, s, &n);
	if (!ft || !t)
		return 0;
	struct sbuf sb = {t, (uint16_t)n, (uint16_t)n};
	int32_t X = (int32_t)x, Y = (int32_t)y, w = text_width(ft, &sb), ch = (int32_t)(ft->img.h >> 4);
	if (X + MASK_X < 0 || X + MASK_X + w > TEXT_IMG_W || Y <= -ch || Y >= (int32_t)S.H)
		return I32(w);
	struct point at = {MASK_X + X, Y};
	uint16_t colours[2] = {0x7FFF, 0}, drawn = 0;
	text_draw_proportional(&S.mask, &at, ft, &sb, colours, &drawn);
	paint((uint32_t)p, MASK_X + X, MASK_X + X + drawn + 1);
	return I32(drawn);
}
static uint64_t h_text_width(struct ebpf_vm *vm, uint64_t f, uint64_t s, uint64_t c, uint64_t d, uint64_t e)
{
	(void)c, (void)d, (void)e;
	struct font *ft = font_(f);
	uint32_t n;
	const uint8_t *t = str(vm, s, &n);
	if (!ft || !t)
		return 0;
	struct sbuf sb = {t, (uint16_t)n, (uint16_t)n};
	return I32(text_width(ft, &sb));
}
static uint64_t h_font_height(struct ebpf_vm *vm, uint64_t f, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	(void)vm, (void)b, (void)c, (void)d, (void)e;
	struct font *ft = font_(f);
	return I32(ft ? (int32_t)(ft->img.h >> 4) : -1);
}
static uint64_t h_font_find(struct ebpf_vm *vm, uint64_t s, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	(void)b, (void)c, (void)d, (void)e;
	uint32_t n;
	const uint8_t *t = str(vm, s, &n);
	if (!t)
		return I32(-1);
	for (int32_t k = 0; k < font_count; k++)
		if (keq(t, n, font_table[k].name))
			return I32(k);
	return I32(-1);
}
int fonts_auto_pick(uint32_t box_h);                        /* fonts.c */
static uint64_t h_font_auto(struct ebpf_vm *vm, uint64_t h, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	(void)vm, (void)b, (void)c, (void)d, (void)e;
	return I32(fonts_auto_pick((uint32_t)(int32_t)h));
}
static uint64_t h_image(struct ebpf_vm *vm, uint64_t x, uint64_t y, uint64_t a, uint64_t fr, uint64_t e)
{
	(void)e;
	uint32_t w, h, frames, ms;
	const uint8_t *px = asset(APP, (int32_t)a, &w, &h, &frames, &ms);
	if (!px || !frames)
		return 0;
	int32_t f = (int32_t)fr % (int32_t)frames;
	if (f < 0)
		f += (int32_t)frames;
	for (uint32_t yy = 0; yy < h; yy++)
		for (uint32_t xx = 0; xx < w; xx++) {
			uint32_t v = be16(px + 2 * (((uint32_t)f * h + yy) * w + xx));
			int32_t dx = (int32_t)x + (int32_t)xx, dy = (int32_t)y + (int32_t)yy;
			if (!(v & 0x8000) && dx >= 0 && dx < (int32_t)S.W && dy >= 0 && dy < (int32_t)S.H)
				S.buf.pixels[(uint32_t)dy * S.W + (uint32_t)dx] = (uint16_t)v;
		}
	return 0;
}
static uint64_t h_asset_find(struct ebpf_vm *vm, uint64_t s, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	(void)b, (void)c, (void)d, (void)e;
	uint32_t n;
	const uint8_t *t = str(vm, s, &n);
	struct app *a = APP;
	if (!t)
		return I32(-1);
	for (uint32_t k = 0; k < a->nassets; k++)
		if (a->as[k].nl == n) {
			uint32_t i = 0;
			while (i < n && a->as[k].name[i] == t[i])
				i++;
			if (i == n)
				return I32(k);
		}
	return I32(-1);
}
static uint64_t h_asset_info(struct ebpf_vm *vm, uint64_t a, uint64_t what, uint64_t c, uint64_t d, uint64_t e)
{
	(void)c, (void)d, (void)e;
	uint32_t v[4];
	if (!asset(APP, (int32_t)a, &v[0], &v[1], &v[2], &v[3]) || (uint32_t)what > 3)
		return I32(-1);
	return I32(v[(uint32_t)what]);
}
static uint64_t h_now(struct ebpf_vm *vm, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	(void)vm, (void)a, (void)b, (void)c, (void)d, (void)e;
	return (uint64_t)now_s();
}
static uint64_t h_fmt_time(struct ebpf_vm *vm, uint64_t buf, uint64_t len, uint64_t fmt, uint64_t t, uint64_t e)
{
	(void)e;
	uint32_t n;
	const uint8_t *f = str(vm, fmt, &n);
	if (!f)
		return 0;
	uint8_t tmp[256];
	struct out o = {tmp, 0, 255};
	strftime_(&o, f, (int64_t)t);
	return I32(out(vm, buf, (int32_t)len, tmp, o.n));
}
static uint64_t h_setting_int(struct ebpf_vm *vm, uint64_t k, uint64_t d, uint64_t c, uint64_t dd, uint64_t e)
{
	(void)c, (void)dd, (void)e;
	uint32_t n, vn = 0;
	const uint8_t *key = str(vm, k, &n);
	const uint8_t *v = key ? setting(APP, key, n, &vn) : 0;
	return I32(parse_int(v, vn, (int32_t)d));
}
static uint64_t h_setting_str(struct ebpf_vm *vm, uint64_t k, uint64_t buf, uint64_t len, uint64_t d, uint64_t e)
{
	(void)d, (void)e;
	uint32_t n, vn = 0;
	const uint8_t *key = str(vm, k, &n);
	const uint8_t *v = key ? setting(APP, key, n, &vn) : 0;
	return I32(v ? out(vm, buf, (int32_t)len, v, vn) : -1);
}
static uint64_t h_data_int(struct ebpf_vm *vm, uint64_t k, uint64_t d, uint64_t c, uint64_t dd, uint64_t e)
{
	(void)c, (void)dd, (void)e;
	uint32_t n;
	const uint8_t *key = str(vm, k, &n);
	struct datum *v = key ? datum(APP, key, n) : 0;
	return I32(v ? parse_int(v->value, v->len, (int32_t)d) : (int32_t)d);
}
static uint64_t h_data_str(struct ebpf_vm *vm, uint64_t k, uint64_t buf, uint64_t len, uint64_t d, uint64_t e)
{
	(void)d, (void)e;
	uint32_t n;
	const uint8_t *key = str(vm, k, &n);
	struct datum *v = key ? datum(APP, key, n) : 0;
	return I32(v ? out(vm, buf, (int32_t)len, v->value, v->len) : -1);
}
static uint64_t h_data_age(struct ebpf_vm *vm, uint64_t k, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	(void)b, (void)c, (void)d, (void)e;
	uint32_t n;
	const uint8_t *key = str(vm, k, &n);
	struct datum *v = key ? datum(APP, key, n) : 0;
	int64_t t = now_s();
	if (!v || t < 0)
		return I32(-1);
	return I32(t - (int64_t)v->at < 0 ? 0 : (int32_t)(t - (int64_t)v->at));
}
static uint64_t h_outline(struct ebpf_vm *vm, uint64_t style, uint64_t colour, uint64_t c, uint64_t d, uint64_t e)
{
	(void)vm, (void)c, (void)d, (void)e;
	int32_t st = (int32_t)style;
	if (st < 1 || st > 3)
		return 0;
	struct rect r = {0, 0, (int32_t)S.W, (int32_t)S.H}, clip = r;
	struct point at = {0, 0};
	int32_t o[2] = {st, (int32_t)((colour & 7) << 16)};
	glyph_outline_blit(&S.omask, &r, &S.buf, &at, o, &clip);
	return 0;
}
static uint64_t h_print(struct ebpf_vm *vm, uint64_t s, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	(void)b, (void)c, (void)d, (void)e;
	uint32_t n;
	const uint8_t *t = str(vm, s, &n);
	if (t)
		console_log_line((const char *)t);
	return 0;
}

static void helpers_fill(void)
{
	ebpf_helper *h = S.helpers;
	h[0] = 0, h[1] = h_width, h[2] = h_height, h[3] = h_clear, h[4] = h_rect, h[5] = h_pixel, h[6] = h_text;
	h[7] = h_text_width, h[8] = h_font_height, h[9] = h_font_find, h[10] = h_image, h[11] = h_asset_find;
	h[12] = h_asset_info, h[13] = h_now, h[14] = h_fmt_time, h[15] = h_setting_int, h[16] = h_setting_str;
	h[17] = h_data_int, h[18] = h_data_str, h[19] = h_data_age, h[20] = h_outline, h[21] = h_print;
	h[22] = h_font_auto;
}

/* ---- pushed data (console "adata") ---- */
int apps_data(unsigned line, const char *key, const uint8_t *value, unsigned n)
{
	struct app *a = line < NLINES ? app_load(line) : 0;
	if (!a)
		return 0;
	unsigned klen = 0;
	while (key[klen] && klen < 15)
		klen++;
	struct datum *d = datum(a, (const uint8_t *)key, klen);
	for (unsigned k = 0; !d && k < NDATA; k++)
		if (!a->data[k].key[0])
			d = &a->data[k];
	if (!d)
		return 0;
	for (unsigned k = 0; k < klen; k++)
		d->key[k] = key[k];
	d->key[klen] = 0;
	if (n > sizeof d->value)
		n = sizeof d->value;
	for (unsigned k = 0; k < n; k++)
		d->value[k] = value[k];
	d->len = n;
	int64_t t = now_s();
	d->at = t < 0 ? 0 : (uint32_t)t;
	return 1;
}

const char *apps_status(unsigned line, int *loaded)
{
	struct app *a = line < NLINES ? app_load(line) : 0;
	*loaded = a != 0;
	return a ? a->error : "";
}

/* ---- the attract loop ---- */
void ext_textline_init(void *tl, rect_t *box, uint8_t line, int alloc, int mode)
{
	textline_init__orig(tl, box, line, alloc, mode);
	S.line_steps = 0;
	S.active = -1;
	S.inits++;
	if (line < NLINES && app_load(line) && images()) {
		S.active = line;
		S.app[line]->t = 0;
	}
}

int32_t ext_textline_step(void *tl, uint32_t effect)
{
	uint8_t line = ((uint8_t *)tl)[4];
	if (S.active < 0 || (int32_t)line != S.active) {
		emoji_anim_time(S.line_steps++);
		return textline_step__orig(tl, effect);
	}
	struct app *a = S.app[line];
	int done;
	for (uint32_t i = 0; i < S.W * S.H; i++)
		S.omask.pixels[i] = 0;
	if (a->error[0]) {                                     /* a failed app: a line saying so, for 3 s */
		struct font *ft = &font_table[0].f;
		uint8_t msg[96];
		struct out o = {msg, 0, sizeof msg - 1};
		puts_(&o, "app ");
		putn(&o, line, 1, '0');
		puts_(&o, ": ");
		puts_(&o, a->error);
		struct sbuf sb = {msg, (uint16_t)o.n, (uint16_t)o.n};
		rect_(0, 0, (int32_t)S.W, (int32_t)S.H, 0);
		struct point at = {MASK_X, (int32_t)(S.H - (ft->img.h >> 4)) / 2};
		uint16_t colours[2] = {0x7FFF, 0}, drawn = 0;
		text_draw_proportional(&S.mask, &at, ft, &sb, colours, &drawn);
		paint(0x7C00, MASK_X, MASK_X + drawn + 1);
		done = a->t >= 120;
	} else {
		emoji_anim_time(a->t);                             /* native_emoji.c: the app's animated emoji */
		a->vm.budget = BUDGET;
		uint64_t args[5] = {a->t, 0, 0, 0, 0};
		uint64_t r = ebpf_run(&a->vm, a->frame_pc, args);
		if (a->vm.error) {
			set_error(a, a->vm.error == EBPF_E_BUDGET ? "out of time" : a->vm.error == EBPF_E_MEMORY ?
			          "memory access" : "stopped", a->vm.error_pc);
			done = 0;
		} else
			done = (uint32_t)r != 0;
	}
	a->t++;
	S.overlay = 1;
	S.runs++;
	return done ? 4 : 3;
}

int update_draw(struct image *frame);                      /* update.c */
void **apps_update_state(void) { return &S.update; }
uint32_t *apps_version_shown(void) { return &S.version_shown; }

void ext_frame_submit(struct image *frame)
{
	S.submits++;
	S.last_lr = 0, S.last_px = (uint32_t)frame->pixels;
	S.last_wh = frame->w << 16 | (frame->h & 0xFFFF);
	if (update_draw(frame)) {                                  /* a firmware update: the panel shows it */
		S.upd_frames++;
		S.overlay = 0;
		frame_submit__orig(frame);
		return;
	}
	if (S.overlay && S.buf.pixels && frame->w == S.W && frame->h == S.H) {
		for (uint32_t i = 0; i < S.W * S.H; i++)
			frame->pixels[i] = S.buf.pixels[i];
		S.overlays++;
	}
	S.overlay = 0;
	frame_submit__orig(frame);
}

/* ---- the attract lines the apps own: on, an empty-looking placeholder text, the effect left alone ---- */
#define APP_EFFECT  0x80008000u                          /* transparent: the app's buffer is the frame */

void apps_apply_lines(void)
{
	for (unsigned n = 0; n < NLINES; n++) {
		int has = app_load(n) != 0;
		if (has || g_attract.effect[n] == APP_EFFECT) {
			g_attract.mode[n] = has;
			g_attract.effect[n] = has ? APP_EFFECT : 0;
			g_attract.outline[n][0] = g_attract.outline[n][1] = 0;
			if (!g_attract.text_set[n])
				g_message_table[n * 256] = has ? ' ' : 0, g_message_table[n * 256 + 1] = 0;
		}
	}
}

void **apps_store_state(void) { return &S.store; }
void apps_store_busy(int busy) { S.store_busy = (uint32_t)busy; }

void apps_debug(uint32_t v[12])
{
	v[0] = S.W, v[1] = S.H, v[2] = (uint32_t)S.active, v[3] = S.inits, v[4] = S.runs, v[5] = S.overlays;
	v[6] = (uint32_t)S.buf.pixels, v[7] = S.submits, v[8] = S.upd_frames;
	v[9] = S.last_lr, v[10] = S.last_px, v[11] = S.last_wh;
}
