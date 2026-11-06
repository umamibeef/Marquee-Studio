/* What the boot flash chip runs when there is no firmware to start: it says so on the panel. Nothing else: no console,
 * no store, no content. The boot loader's DB9 window, which opens at every start, is how new firmware gets in.
 * Why it is here: BOOT_TRIES at 0xFF means the update slot failed its tries (bootloader/src/loader.c). */
#include <stdint.h>
#include "hw.h"
#include "dbg.h"
#include "failsafe_font.h"

#define W 200
#define H 16
#define RED 0x7C00u
#define MSG_MAX 96

uint32_t crash_regs[38];                       /* start.S saves here; a crash here just lets the watchdog restart us */
void crash_report(uint32_t vector)
{
	(void)vector;
	for (;;)
		;
}

static uint16_t px[W * H];
static char msg[MSG_MAX];

static int put(int n, const char *s)
{
	while (*s && n < MSG_MAX - 1)
		msg[n++] = *s++;
	msg[n] = 0;
	return n;
}

static int put_hex(int n, uint32_t v)
{
	char s[9];
	for (int i = 0; i < 8; i++)
		s[i] = "0123456789ABCDEF"[v >> (28 - 4 * i) & 15];
	s[8] = 0;
	return put(n, s);
}

static void compose_message(void)
{
	uint32_t base = *(volatile uint32_t *)0x24, size = *(volatile uint32_t *)0x28;
	const volatile uint8_t *end = (const volatile uint8_t *)(base + size);
	int n, failed = size && end[-0x10] == 0xFF;

	n = put(0, failed ? "FIRMWARE FAILED TO START - INSTALL OVER DB9" : "NO FIRMWARE INSTALLED - INSTALL OVER DB9");
	if (failed && size >= DBG_OFFSET + 0x200u) {
		const volatile struct dbg_crash *c = (const volatile struct dbg_crash *)(base + DBG_OFFSET + 0x100u);
		if (c->magic == DBG_CRASH_MAGIC) {
			n = put(n, " - CRASH AT ");
			put_hex(n, c->srr0);
		}
	}
}

static const unsigned char *glyph(char c)
{
	for (unsigned i = 0; font_chars[i]; i++)
		if (font_chars[i] == c)
			return font_rows[i];
	return font_rows[0];
}

static void draw(int x0, int chars)
{
	for (int i = 0; i < W * H; i++)
		px[i] = 0;
	for (int k = 0; k < chars; k++) {
		int cx = x0 + 8 * k;
		if (cx <= -8 || cx >= W)
			continue;
		const unsigned char *g = glyph(msg[k]);
		for (int y = 0; y < H; y++)
			for (int b = 0; b < 8; b++)
				if ((g[y] & (0x80u >> b)) && cx + b >= 0 && cx + b < W)
					px[y * W + cx + b] = RED;
	}
}

void app_main(void)
{
	int chars = 0;

	wdt_kick();
	compose_message();
	while (msg[chars])
		chars++;
	if (fpga_load())
		for (;;)
			wdt_kick();                        /* no panel to say it on */
	panel_init();
	for (;;)
		for (int x = W; x > -8 * chars; x--) {
			do
				wdt_kick();
			while (!panel_ready());
			draw(x, chars);
			panel_present(px, W, H);
		}
}
