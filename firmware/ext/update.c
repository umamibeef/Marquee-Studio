/* Firmware updates while the sign keeps running, shown on its panel.
 *
 * The boot loader's INSTALL (bootloader/src/serdump.c) writes an image into the update slot from its serial window,
 * with the panel dark. This does the same from our running firmware, over the DB9 console, and the panel shows it:
 *   ubegin SIZE CRC   a RAM buffer for the image (SIZE <= 512 KB, CRC its CRC-32); the panel shows the update
 *   uraw N            then N raw bytes of the image (the console's raw mode, as for the content store)
 *   ucommit           checks the CRC, erases and programs the update slot, reads it back, clears the slot's try
 *                     counter and flags (NVRAM 0x7FF0 / 0x7FF1, as INSTALL does) and restarts the sign: the boot
 *                     loader boots the slot, falling back to the boot flash's firmware by itself if it fails
 *   uabort            drops the buffer (the panel goes back to normal)
 * The slot is where the boot loader keeps it: the last 512 KB of the main flash, just past the firmware's own
 * download volume (volume 1: the boot loader hands it over without the slot and the loader sector), on a 512 KB
 * boundary. The writes are store.c's (interrupts off per word / sector, the watchdog kicked), so the panel stops
 * for a moment during each sector erase.
 *
 * The panel: "Updating firmware", the step and a percentage in the sign's small automatic font (fonts.auto), and a
 * 3-pixel animated progress bar along the bottom (ext_frame_submit in apps.c calls update_draw). */
#include <stdint.h>

struct image { uint32_t format, w, h; uint16_t *pixels; uint16_t *palette; };
struct font { uint32_t layout; struct image img; uint8_t left[256], width[256], right[256], proportional[256]; };
struct font_entry { uint8_t f[0x618]; char name[256]; };
struct sbuf { const uint8_t *data; uint16_t cap, len; };
struct point { int32_t x, y; };

void *malloc(unsigned long size);
void free(void *p);
void text_draw_proportional(struct image *dst, struct point *pos, struct font *f, struct sbuf *t, uint16_t *colours,
                            uint16_t *width_out);
int fonts_auto_pick(uint32_t box_h);                        /* fonts.c */
extern struct font_entry font_table[100];
extern uint8_t font_count;
extern uint32_t lfs_vol_base[3], lfs_vol_size[3];
int store_erase_sector(uint32_t a);                         /* store.c */
int store_program(uint32_t a, const uint8_t *p, uint32_t n);
void **apps_update_state(void);                            /* apps.c: our state's pointer */
void frame_submit(struct image *frame);                     /* (taken over: ext_frame_submit, then the panel) */
uint32_t panel_width(void);
uint32_t panel_height(void);
uint32_t display_width(void);
uint32_t display_height(void);
uint32_t panel_frames_sent(void);
#define PANEL_FRAMES panel_frames_sent()                     /* frames the panel driver has sent out */

#define SLOT_SIZE 0x80000u
#define SECTOR    0x10000u
#define NVRAM     (*(volatile uint32_t *)0x24)             /* the boot loader's hand-over: NVRAM's address */

enum { RECEIVE, CHECK, ERASE, WRITE, VERIFY, RESTART, FAILED };
static const char *step_name(uint32_t step)                 /* (a switch: no relocated table) */
{
	switch (step) {
	case RECEIVE: return "Receiving";
	case CHECK: return "Checking";
	case ERASE: return "Erasing";
	case WRITE: return "Writing";
	case VERIFY: return "Verifying";
	case RESTART: return "Restarting";
	default: return "Update failed";
	}
}

struct update {
	uint8_t *buf;
	uint32_t size, crc, got, step, done, total, tick;
	uint32_t disp, own, in_own;   /* panel frame counts at the display loop's last frame and at our own; drawing ours */
	struct image img;             /* our own frame, when the display loop draws none (nothing playing) */
};
#define U ((struct update *)*apps_update_state())

static uint32_t crc32_of(const uint8_t *p, uint32_t n)       /* CRC-32 (zlib's) */
{
	uint32_t c = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
	}
	return ~c;
}

/* the update slot, or 0 when the main flash does not look as the boot loader leaves it */
static uint32_t slot_addr(void)
{
	uint32_t end = lfs_vol_base[1] + lfs_vol_size[1];
	uint32_t s = (end + SLOT_SIZE - 1) & ~(SLOT_SIZE - 1);
	if (lfs_vol_base[1] != 0x50000000u || !lfs_vol_size[1] || s - end > SECTOR || s + SLOT_SIZE > 0x52000000u)
		return 0;
	return s;
}

void update_abort(void);

const char *update_begin(uint32_t size, uint32_t crc)
{
	if (!size || size > SLOT_SIZE)
		return "? size: 1..80000";
	if (!slot_addr())
		return "? no update slot (the main flash is not as our boot loader leaves it)";
	update_abort();
	struct update *u = malloc(sizeof *u);
	uint8_t *b = u ? malloc(size) : 0;
	if (!b) {
		free(u);
		return "? out of memory";
	}
	u->buf = b, u->size = size, u->crc = crc, u->got = 0, u->step = RECEIVE, u->done = 0, u->total = size, u->tick = 0;
	u->disp = u->own = PANEL_FRAMES, u->in_own = 0;
	u->img.format = 1, u->img.w = panel_width(), u->img.h = panel_height(), u->img.palette = 0;
	u->img.pixels = malloc(u->img.w * u->img.h * 2);              /* none: only the display loop's frames show it */
	*apps_update_state() = u;
	return 0;
}

/* The update on the panel even when nothing plays: the display loop sends no frames then (all lines off, no app),
 * so our hook in it never runs. Called as the work goes on (each received chunk, each sector): when the display
 * loop has sent nothing for 3 panel frames, our own frame goes out, at most one per panel frame. */
#define UPDATE_FONT "7x13B.mqf"

/* the font table index of the font with this name, or -1 (bootscreen.c uses it too) */
int font_named(const char *name)
{
	for (int i = 0; i < font_count && i < 100; i++) {
		const char *a = font_table[i].name, *b = name;
		while (*a && *a == *b)
			a++, b++;
		if (!*a && !*b)
			return i;
	}
	return -1;
}

static void update_show(struct update *u)
{
	uint32_t f = PANEL_FRAMES;
	if (!u->img.pixels || f - u->disp < 3 || f == u->own)
		return;
	u->own = f, u->in_own = 1;
	frame_submit(&u->img);
	u->in_own = 0;
}

/* raw bytes of the image (console.c's raw mode); nonzero when there is no update or it is already full */
int update_data(const uint8_t *p, uint32_t n)
{
	struct update *u = U;
	if (!u || u->step != RECEIVE || u->got + n > u->size)
		return -1;
	for (uint32_t k = 0; k < n; k++)
		u->buf[u->got++] = p[k];
	u->done = u->got;
	update_show(u);
	return 0;
}

void update_abort(void)
{
	if (U) {
		free(U->img.pixels);
		free(U->buf);
		free(U);
		*apps_update_state() = 0;
	}
}

static const char *fail(struct update *u, const char *why)
{
	u->step = FAILED;
	u->own = PANEL_FRAMES - 1;
	update_show(u);
	return why;
}

/* everything after the last byte: 0 when the sign is about to restart into the new firmware */
const char *update_commit(void)
{
	struct update *u = U;
	uint32_t slot = slot_addr();
	if (!u || u->step != RECEIVE)
		return "? no update (ubegin first)";
	if (u->got != u->size)
		return "? not all bytes received";
	u->step = CHECK, u->done = 0, u->total = 1;
	if (crc32_of(u->buf, u->size) != u->crc)
		return fail(u, "? CRC does not match: nothing written");
	u->step = ERASE, u->done = 0, u->total = SLOT_SIZE / SECTOR;
	for (uint32_t s = 0; s < SLOT_SIZE; s += SECTOR, u->done++) {
		update_show(u);
		if (store_erase_sector(slot + s))
			return fail(u, "? erase failed (the slot is now empty: the sign boots its boot flash)");
	}
	u->step = WRITE, u->done = 0, u->total = u->size;
	for (uint32_t k = 0; k < u->size; k += 1024) {
		uint32_t n = u->size - k < 1024 ? u->size - k : 1024;
		if (store_program(slot + k, u->buf + k, n))
			return fail(u, "? program failed (the slot will not boot: the sign boots its boot flash)");
		u->done = k + n;
		update_show(u);
	}
	u->step = VERIFY, u->done = 0, u->total = 1;
	if (crc32_of((const uint8_t *)slot, u->size) != u->crc)
		return fail(u, "? read-back CRC differs (the slot will not boot: the sign boots its boot flash)");
	*(volatile uint8_t *)(NVRAM + 0x7FF0) = 0;               /* as INSTALL: the next boot tries the slot */
	*(volatile uint8_t *)(NVRAM + 0x7FF1) = 0;
	u->step = RESTART, u->done = 1, u->total = 1;
	u->own = PANEL_FRAMES - 1;                                 /* "restarting" goes out now */
	update_show(u);
	return 0;
}

/* the panel while an update runs: 1 when it drew the frame */
int update_draw(struct image *frame)
{
	struct update *u = U;
	if (!u || !frame->pixels)
		return 0;
	if (!u->in_own)
		u->disp = PANEL_FRAMES;
	/* the frame can be larger than the display (the real sign: 240 x 128 frames, its 200 x 16 LEDs top left): lay
	 * out by the display's size, address by the frame's width */
	uint32_t S = frame->w, W = display_width(), H = display_height();
	if (W > frame->w)
		W = frame->w;
	if (H > frame->h)
		H = frame->h;
	for (uint32_t i = 0; i < frame->w * frame->h; i++)
		frame->pixels[i] = 0;
	uint32_t pct = u->total ? (uint32_t)((uint64_t)u->done * 100 / u->total) : 0;
	/* the overall progress: receiving is the first 70 %, erasing 10, writing 18, the rest 2 */
	static const uint8_t base[] = {0, 70, 70, 80, 98, 100, 0}, span[] = {70, 0, 10, 18, 2, 0, 0};
	uint32_t all = u->step == FAILED ? 0 : base[u->step] + span[u->step] * pct / 100;
	char msg[48];
	const char *s = step_name(u->step);                     /* short: 7x13B is 7 px a character */
	uint32_t n = 0;
	while (*s && n < 40)
		msg[n++] = *s++;
	if (u->step != FAILED && u->step != RESTART) {
		msg[n++] = ' ';
		if (all >= 100)
			msg[n++] = '1';
		if (all >= 10)
			msg[n++] = (char)('0' + all / 10 % 10);
		msg[n++] = (char)('0' + all % 10);
		msg[n++] = '%';
	}
	int fi = font_named(UPDATE_FONT);                      /* 13 px: fills the room above the bar */
	if (fi < 0)
		fi = fonts_auto_pick(H > 4 ? H - 4 : H);
	if (fi >= 0 && fi < font_count) {
		struct font *f = (struct font *)font_table[fi].f;
		struct sbuf t = {(const uint8_t *)msg, (uint16_t)n, (uint16_t)n};
		uint16_t colours[2] = {(uint16_t)(u->step == FAILED ? 0x7C00 : 0x7FE0), 0}, w = 0;
		uint32_t ch = f->img.h >> 4;
		struct point at = {2, (int32_t)((H - 3 > ch ? H - 3 - ch : 0) / 2)};
		text_draw_proportional(frame, &at, f, &t, colours, &w);
	}
	/* the bar, 3 rows along the bottom: its filled part a green gradient that runs back against the fill (right to
	 * left, one pixel every 3 frames: about 13 pixels a second); the rest of the rows off */
	uint32_t bar = W * all / 100, phase = u->tick++ / 3;
	for (uint32_t x = 0; x < W; x++) {
		uint32_t p = (x + phase) & 31, tri = p < 16 ? p : 31 - p;     /* 0..15..0 over 32 pixels */
		uint16_t c = x < bar ? (uint16_t)((6 + tri * 25 / 15) << 5) : 0;
		for (uint32_t y = H - 3; y < H; y++)
			frame->pixels[y * S + x] = c;
	}
	return 1;
}
