/* The application core: what sits under the apps, fonts, emoji, store and console. No RTOS: one loop that serves the
 * serial lines while the panel is busy and draws one frame whenever the panel has taken the last. */
#include <stdint.h>
#include "hw.h"
#include "serial.h"
#include "serial2.h"
#include "core.h"
#include "engine/engine.h"
#include "dbg.h"
#include "delivery.h"

void *malloc(unsigned long size);
void *memset(void *d, int c, unsigned long n);
struct lfs_file { const uint8_t *data; uint32_t size, f8, flags, f10, pos, f18, f1c, f20, name, index, f2c; };
int store_find(const char *name, const uint8_t **data, uint32_t *size);

/* the sign: an AD0501, its LEDs counted as 200 x 16 (the FPGA's frame is larger: hw.c) */
#define DISPLAY_W 200u
#define DISPLAY_H 16u

struct attract_cfg g_attract;
uint8_t g_message_table[256 * 17];

uint32_t display_width(void) { return DISPLAY_W; }
uint32_t display_height(void) { return DISPLAY_H; }
uint32_t panel_width(void) { return DISPLAY_W; }
uint32_t panel_height(void) { return DISPLAY_H; }

/* ---- the clock ---- */
uint32_t clock_ticks(void) { return (uint32_t)(time_ticks() / (time_hz() / CLOCK_TPS)); }

/* ---- archive volumes: 0 the boot flash, 1 the main flash (without the update slot) ---- */
uint32_t lfs_vol_base[3], lfs_vol_size[3];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* a file of the boot image's archive. 1 if found. */
int lfs_open(struct lfs_file *fh, const char *name)
{
	uint32_t a = lfs_vol_base[0], end = a + lfs_vol_size[0];
	while (a + 16 <= end) {
		const uint8_t *p = (const uint8_t *)a;
		uint32_t n = be32(p), nl = p[12], k = 0;
		if (n < 16 || n == 0xFFFFFFFFu || a + n > end)
			return 0;
		while (k < nl && name[k] && name[k] == (char)p[13 + k])
			k++;
		if (k == nl && !name[k]) {
			fh->data = p + ((nl + 16u) & ~3u);
			fh->size = n - ((nl + 16u) & ~3u) - 4u;
			{ uint32_t sz = be32(p + 4); if (sz <= fh->size) fh->size = sz; }
			return 1;
		}
		a = (a + n + 3u) & ~3u;
	}
	return 0;
}

int lfs_open_bytes(const char *name, const uint8_t **data, uint32_t *size)
{
	struct lfs_file fh;
	if (!lfs_open(&fh, name))
		return 0;
	*data = fh.data, *size = fh.size;
	return 1;
}

/* ---- frames: a ring of 8. A frame is not cleared: an effect word whose background is transparent leaves what the
 * oldest frame held, which is how the sign's effects look. ---- */
#define NFRAMES 8
static uint16_t g_frames[NFRAMES][DISPLAY_W * DISPLAY_H];
static uint32_t g_framek;

void frame_acquire(struct image *frame)
{
	frame->format = 1, frame->w = DISPLAY_W, frame->h = DISPLAY_H;
	frame->pixels = g_frames[g_framek++ % NFRAMES], frame->palette = 0;
}

/* the panel output path, under ext_frame_submit (apps.c) */
void frame_submit__orig(struct image *frame)
{
	static uint32_t healthy;
	panel_present(frame->pixels, (int)frame->w, (int)frame->h);
	if (healthy < 1200 && ++healthy == 1200)
		slot_healthy();
}
void ext_frame_submit(struct image *frame);
void frame_submit(struct image *frame) { ext_frame_submit(frame); }

/* ---- booted from the update slot: tell the boot loader the image works. It counts the tries in NVRAM: BOOT_TRIES (0x10
 * below the NVRAM's end) and its own bit 1 in the flags byte (0xF below the end). Without this, the 4th boot would give
 * up on a good image. ---- */
#define SLOT_FLAG 0x02
void fw_step(uint16_t step, uint32_t value);
void slot_healthy(void)
{
	volatile uint8_t *end = (volatile uint8_t *)(*(volatile uint32_t *)0x24 + *(volatile uint32_t *)0x28);
	if (end[-0xF] & SLOT_FLAG) {
		end[-0x10] = 0;
		end[-0xF] = (uint8_t)(end[-0xF] & ~SLOT_FLAG);
	}
	fw_step(STEP_FW_HEALTHY, 0);
}

/* ---- the serial lines, served between frames ---- */
int lfs_open_bytes(const char *name, const uint8_t **data, uint32_t *size);
void console_rx(int c);
int console_is_open(void);

static void poll_serial(void)
{
	char b[512];
	int n = serial_read(b, (int)sizeof b);
	for (int i = 0; i < n; i++) {
		if (!console_is_open())
			delivery_rx(0, (uint8_t)b[i]);
		console_rx((uint8_t)b[i]);
	}
	n = scc_read(b, (int)sizeof b);
	for (int i = 0; i < n; i++)
		delivery_rx(1, (uint8_t)b[i]);
}

void apps_boot(void);
void apps_apply_lines(void);
void ext_fonts_load(void);
int font_named(const char *name);
int fonts_auto_pick(uint32_t box_h);

/* wait for the panel to take the last frame, serving the serial lines meanwhile */
static void wait_panel(void)
{
	do {
		wdt_kick();
		poll_serial();
	} while (!panel_ready());
}

/* ---- start-up: the version centred for 5 s and a 1 s fade ---- */
#define VERSION_FRAMES 200
#define VERSION_FADE   40
#ifndef FW_VERSION
#define FW_VERSION "?"
#endif

static void boot_screen(void)
{
	int fi = font_named("7x13B.mqf");
	if (fi < 0)
		fi = fonts_auto_pick(DISPLAY_H);
	if (fi < 0)
		return;
	struct font *f = &font_table[fi].f;
	const char *v = FW_VERSION;
	uint16_t n = 0;
	while (v[n])
		n++;
	struct sbuf t = {(const uint8_t *)v, n, n};
	for (int k = 0; k < VERSION_FRAMES + VERSION_FADE; k++) {
		struct image frame;
		wait_panel();
		frame_acquire(&frame);
		memset(frame.pixels, 0, DISPLAY_W * DISPLAY_H * 2);
		int32_t w = text_width(f, &t), ch = (int32_t)(f->img.h >> 4);
		struct point at = {((int32_t)DISPLAY_W - w) / 2, ((int32_t)DISPLAY_H - ch) / 2};
		uint32_t lv = k < VERSION_FRAMES ? 31 : 31 * (uint32_t)(VERSION_FRAMES + VERSION_FADE - k) / VERSION_FADE;
		uint16_t colours[2] = {(uint16_t)(lv << 10 | lv << 5), 0}, wo = 0;
		text_draw_proportional(&frame, &at, f, &t, colours, &wo);
		frame_submit(&frame);
	}
}

/* ---- the attract loop (sign-emu attract.ts): the lines that are on, each once, a blank frame between
 * two, blank when no line is on. A line's app runs through ext_textline_init / _step and ext_frame_submit (apps.c). ---- */
static struct textline g_tl;
static int32_t g_cur = -1;
static uint32_t g_have_tl, g_alloc, g_gap, g_line_t;
static struct image g_blank;

static int attract_next(void)
{
	for (int32_t k = 1; k <= NLINES_ATTRACT; k++) {
		int32_t i = (g_cur + k + NLINES_ATTRACT) % NLINES_ATTRACT;
		if (!g_attract.mode[i])
			continue;
		delivery_refresh_line((unsigned)i);
		if (!g_message_table[256 * i])
			continue;
		rect_t box = {0, 0, DISPLAY_W, DISPLAY_H};
		g_cur = i;
		textline_init(&g_tl, &box, (uint8_t)i, !g_alloc, (int)g_attract.outline[i][0]);
		g_alloc = 1, g_have_tl = 1, g_line_t = 0;
		return 1;
	}
	g_cur = -1;
	g_have_tl = 0;
	return 0;
}

static void show_blank(struct image *frame)       /* nothing to show: the panel stays dark */
{
	memset(frame->pixels, 0, DISPLAY_W * DISPLAY_H * 2);
}

static void attract_frame(void)
{
	struct image frame;
	frame_acquire(&frame);
	if (g_gap || (!g_have_tl && !attract_next())) {
		g_gap = 0;
		show_blank(&frame);
		effect_frame_count++;
		frame_submit(&frame);
		return;
	}
	int32_t cur = g_cur;
	struct attract_line l = {&g_message_table[256 * cur], 255, g_attract.effect[cur],
	                         (uint8_t)g_attract.outline[cur][0], (uint8_t)(g_attract.outline[cur][1] >> 16), 0};
	emoji_anim_time(g_line_t++);
	int32_t st = textline_step(&g_tl, l.effect);
	attract_compose(&frame, &g_tl.line_img, &g_tl.f, &l, &g_blank);
	effect_frame_count++;
	frame_submit(&frame);
	/* The line finished: go straight to the next so a looping message and its background run unbroken (no black
	 * frame between loops, which the Studio's free-running background also expects). Only when nothing is left to
	 * show (attract_next found no line) do we fall to a blank frame. */
	if (st == 4 && !attract_next())
		g_gap = 1;
}

/* a store's effectNN.flc plays in slot NN */
static int slot_from_store(unsigned n, const uint8_t **data, uint32_t *size)
{
	char name[] = "effect00.flc";
	name[6] = (char)('0' + n / 10 % 10), name[7] = (char)('0' + n % 10);
	return store_find(name, data, size);
}

void gdb_breakpoint(void);

void app_main(void)
{
	wdt_kick();
	time_init();
	serial_init(115200u);
	scc_init(19200u);
	fw_step(STEP_FW_START, 0);

	lfs_vol_base[0] = *(volatile uint32_t *)0x2C, lfs_vol_size[0] = *(volatile uint32_t *)0x30;
	/* the main flash, both chips (hand-off 0x3C..0x48); the boot loader leaves its update slot and loader sector out */
	lfs_vol_base[1] = *(volatile uint32_t *)0x3C;
	lfs_vol_size[1] = *(volatile uint32_t *)0x40 + *(volatile uint32_t *)0x48;
	mmu_map_main_flash();
	fw_step(STEP_FW_VOLUMES, 0);

	const char *fe = fpga_load();
	if (fe) {
		serial_puts(fe);
		serial_puts("\r\n");
	}
	panel_init();
	fw_step(STEP_FW_LAYOUT, 0);

	message_table = g_message_table;
	message_table_size = sizeof g_message_table;
	g_blank.format = 1, g_blank.w = DISPLAY_W, g_blank.h = DISPLAY_H;
	g_blank.pixels = malloc(DISPLAY_W * DISPLAY_H * 2);
	memset(g_blank.pixels, 0, DISPLAY_W * DISPLAY_H * 2);
	effect_slot_set_provider(slot_from_store);

	apps_boot();
	ext_fonts_load();
	apps_apply_lines();
	delivery_boot();
	boot_screen();

	for (;;) {
		wait_panel();
		attract_frame();
	}
}
