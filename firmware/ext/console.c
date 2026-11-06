/* Serial console on the DB9. Closed until its magic line, "MQ?MON" and CR, has come; then it reads lines and answers
 * until "exit". console_rx takes one received byte and returns at once: the main loop feeds it between frames, so the
 * panel keeps running while the console is open. */
#include <stdint.h>
#include "../../bootloader/include/dbg.h"

#include "../app/serial.h"
#include "../app/hw.h"
extern char __heap_start[];

struct emoji_state { uint32_t tried, size, count; const uint16_t *palette; const uint8_t *pixels, *map; uint32_t map_len; };
extern struct emoji_state emoji_state;
void emoji_reset(void);

#define RAM_TOP     (*(volatile uint32_t *)0x14)      /* hand-off block: top of RAM */
#define BOOT_FLASH  0xF8180000u
#define NVRAM       0xF8780000u
#define REGION_512K 0x80000u
#define NBUF  16
#define BUFSZ 256
#define LINE  64

struct console_state {
	uint32_t open, magic, len, cur, used;
	uint8_t *bufs;
	char line[LINE];
	uint32_t raw, rn, rerr;                      /* "sraw": bytes still to come, in rbuf, a refused piece */
	uint32_t rdest;                              /* where they go: 0 the content store, 1 a firmware update */
	uint8_t rbuf[16];
};
struct console_state console_state;
static uint8_t g_outbuf[NBUF * BUFSZ];
#define C console_state

static const char MAGIC[] = "MQ?MON\r";
static const char HEX[] = "0123456789abcdef";

static __attribute__((noinline)) void flush(void)
{
	if (!C.used)
		return;
	serial_write((const char *)C.bufs + C.cur * BUFSZ, (int)C.used);   /* the driver copies it and flushes the cache */
	C.cur = (C.cur + 1) % NBUF;
	C.used = 0;
}

static __attribute__((noinline)) void out(char ch)
{
	C.bufs[C.cur * BUFSZ + C.used++] = (uint8_t)ch;
	if (C.used == BUFSZ)
		flush();
}

static __attribute__((noinline)) void outs(const char *s)
{
	while (*s)
		out(*s++);
}

static __attribute__((noinline)) void outhex(uint32_t v, int digits)
{
	while (digits--)
		out(HEX[v >> (4 * digits) & 15]);
}

/* A hex number at *s (optional 0x), advancing *s past it and the spaces after; *ok cleared when there is none. */
static __attribute__((noinline)) uint32_t hexarg(char **s, int *ok)
{
	uint32_t v = 0;
	int n = 0;
	char *p = *s;

	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	for (;; p++, n++) {
		char c = *p;
		unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
		           : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;
		if (d > 15)
			break;
		v = v << 4 | d;
	}
	while (*p == ' ')
		p++;
	if (!n)
		*ok = 0;
	*s = p;
	return v;
}

/* [a, a + n) inside RAM, the boot flash or the NVRAM. */
static int readable(uint32_t a, uint32_t n)
{
	uint32_t e = a + n;

	if (e < a)
		return 0;
	return e <= RAM_TOP || (a >= BOOT_FLASH && e <= BOOT_FLASH + REGION_512K) ||
	       (a >= NVRAM && e <= NVRAM + REGION_512K) || (a >= 0x50000000u && e <= 0x52000000u) ||   /* + main flash */
	       (a >= 0xFF000000u && e <= 0xFF010000u);   /* + the IMMR register block, for on-hardware md debugging */
}

static __attribute__((noinline)) void cmd_md(uint32_t a, uint32_t n)
{
	uint32_t i, j;

	if (!readable(a, n)) {
		outs("? range: RAM, boot flash f8180000+80000, NVRAM f8780000+80000, main flash 50000000+2000000\r\n");
		return;
	}
	for (i = 0; i < n; i += 16) {
		outhex(a + i, 8);
		out(':');
		for (j = 0; j < 16 && i + j < n; j++) {
			out(' ');
			outhex(*(volatile uint8_t *)(a + i + j), 2);
		}
		for (; j < 16; j++)
			outs("   ");
		outs("  ");
		for (j = 0; j < 16 && i + j < n; j++) {
			uint8_t c = *(volatile uint8_t *)(a + i + j);
			out(c >= 0x20 && c < 0x7F ? (char)c : '.');
		}
		outs("\r\n");
	}
}

/* ---- programmable apps (apps.c) and the content store (store.c): the upload over the DB9 ---- */
void apps_set_time(uint32_t secs);
uint32_t apps_now(int *valid);
const char *apps_status(unsigned line, int *loaded);
int apps_data(unsigned line, const char *key, const uint8_t *value, unsigned n);
void store_begin(void);
const char *store_file(const char *name, uint32_t size);
const char *store_data(const uint8_t *p, uint32_t n);
const char *update_begin(uint32_t size, uint32_t crc);      /* update.c */
int update_data(const uint8_t *p, uint32_t n);
const char *update_commit(void);
void update_abort(void);
const char *store_commit(void);
uint32_t store_list(void (*each)(const char *name, uint32_t nl, uint32_t size));

/* an app's print() (apps.c): a line on the console, when it is open */
void console_log_line(const char *s)
{
	if (!C.open)
		return;
	outs("log: ");
	outs(s);
	outs("\r\n");
}

/* "HEX..." -> bytes, at most max; -1 on a bad digit or an odd count */
static __attribute__((noinline)) int hexbytes(const char *p, uint8_t *out, unsigned max)
{
	unsigned n = 0;
	while (*p == ' ')
		p++;
	for (; p[0] && p[0] != ' '; p += 2) {
		int v = 0;
		for (int k = 0; k < 2; k++) {
			char c = p[k];
			int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (d < 0)
				return -1;
			v = v << 4 | d;
		}
		if (n >= max)
			return -1;
		out[n++] = (uint8_t)v;
	}
	return (int)n;
}

static void list_one(const char *name, uint32_t nl, uint32_t size)
{
	outs("file ");
	for (uint32_t k = 0; k < nl; k++)
		out(name[k]);
	outs(" ");
	outhex(size, 6);
	outs("\r\n");
}

void apps_debug(uint32_t v[12]);

static __attribute__((noinline)) void cmd_apps(void)
{
	int valid, loaded;
	uint32_t dv[12];
	apps_debug(dv);
	outs("panel ");
	for (int k = 0; k < 12; k++) {
		outhex(dv[k], 8);
		out(' ');
	}
	outs("(w h active inits runs overlays buf submits update-frames last-caller last-pixels last-wh)\r\n");
	uint32_t now = apps_now(&valid);
	outs("time ");
	if (valid)
		outhex(now, 8);
	else
		outs("not set");
	outs("\r\n");
	for (unsigned n = 0; n < 16; n++) {
		const char *err = apps_status(n, &loaded);
		if (!loaded)
			continue;
		outs("app line ");
		outhex(n, 1);
		outs(err[0] ? " error: " : " ok");
		outs(err);
		outs("\r\n");
	}
}

/* The NVRAM debug record (bootloader/include/dbg.h); tools/dbgrecord.py has the names of the steps. */
static __attribute__((noinline)) void cmd_boots(void)
{
	volatile struct dbg_area *d = (volatile struct dbg_area *)(*(volatile uint32_t *)0x24 + DBG_OFFSET);
	volatile struct dbg_crash *c = &d->crash;
	uint32_t i, k;

	if (d->magic != DBG_MAGIC) {
		outs("no debug record (not our boot loader)\r\n");
		return;
	}
	outs("boot ");
	outhex(d->seq, 8);
	outs(" options ");
	outhex(d->options, 2);
	outs("\r\n  boot     rsr      step value\r\n");
	for (k = 1; k <= DBG_BOOTS; k++) {                       /* oldest first */
		volatile struct dbg_boot *b = &d->boots[(d->seq + k) & (DBG_BOOTS - 1)];

		if (!b->seq)
			continue;
		outs("  ");
		outhex(b->seq, 8);
		out(' ');
		outhex(b->rsr, 8);
		out(' ');
		outhex(b->step, 4);
		out(' ');
		outhex(b->value, 8);
		outs("\r\n");
	}
	if (c->magic != DBG_CRASH_MAGIC) {
		outs("no crash recorded\r\n");
		return;
	}
	outs("crash in boot ");
	outhex(c->seq, 8);
	outs(" source ");
	outhex(c->source, 1);
	outs(" vector ");
	outhex(c->vector, 4);
	outs(" step ");
	outhex(c->step, 2);
	outs("\r\n  srr0 ");
	outhex(c->srr0, 8);
	outs(" srr1 ");
	outhex(c->srr1, 8);
	outs(" dar ");
	outhex(c->dar, 8);
	outs(" dsisr ");
	outhex(c->dsisr, 8);
	outs(" lr ");
	outhex(c->lr, 8);
	outs("\r\n");
	for (i = 0; i < 32; i++) {
		outhex(c->gpr[i], 8);
		outs((i & 7) == 7 ? "\r\n" : " ");
	}
}

static __attribute__((noinline)) void cmd_info(void)
{
	outs("firmware " FW_VERSION "\r\nram top ");
	outhex(RAM_TOP, 8);
	outs(", heap at ");
	outhex((uint32_t)__heap_start, 8);
	outs("\r\nemoji: ");
	outhex(emoji_state.count, 4);
	outs(" cells of ");
	outhex(emoji_state.size, 2);
	outs(emoji_state.tried ? " (loaded)" : " (not loaded yet)");
	outs("\r\nconsole state at ");
	outhex((uint32_t)&C, 8);
	{
		/* RSR (IMMR+0x288): why the CPU reset since power-on (bits are kept until written back with 1s) */
		static const char NAMES[8][9] = {"ext-hard", "ext-soft", "pll-lock", "sw-wdog", "chkstop", "dbg-hard",
		                                 "dbg-soft", "jtag"};
		uint32_t rsr = *(volatile uint32_t *)0xFF000288, i;

		outs("\r\nreset status (RSR) ");
		outhex(rsr, 8);
		for (i = 0; i < 8; i++)
			if (rsr & (0x80000000u >> i)) {
				out(' ');
				outs(NAMES[i]);
			}
	}
	outs("\r\n");
}

static __attribute__((noinline)) void exec(char *s)
{
	int ok = 1;
	char *p = s;
	uint32_t a, v;

	while (*p && *p != ' ')
		p++;
	while (*p == ' ')
		*p++ = 0;
	if (!*s)
		return;
	if (!__builtin_strcmp(s, "help")) {
		outs("md ADDR [LEN]   hex dump (RAM, boot flash, NVRAM)\r\n"
		     "mw ADDR WORD    write a word to RAM\r\n"
		     "info            RAM, heap, emoji, console\r\n"
		     "boots           the last boots and the last crash (NVRAM debug record)\r\n"
		     "apps            the apps on the sign (app0..f.mqap in the store) and their errors\r\n"
		     "time SECS       set the sign's clock (hex, local wall-clock seconds since 1970)\r\n"
		     "adata L KEY HEX send a value to the app on line L (bytes in hex)\r\n"
		     "sbegin          start a new content store (sfile NAME SIZE, then sd HEX..., per file)\r\n"
		     "sraw SIZE       then SIZE raw bytes of the current file (faster than sd)\r\n"
		     "scommit         write the staged files to the main flash (replaces the store)\r\n"
		     "sls             the files in the store\r\n"
		     "ubegin SIZE CRC a firmware update, shown on the panel (uraw SIZE + bytes, then ucommit)\r\n"
		     "ucommit         write it into the update slot and restart into it; uabort drops it\r\n"
		     "nv OFF BYTE     write a byte in the NVRAM's reserved top (7c00..7fff: 7ff0 = slot tries)\r\n"
		     "reset           restart the sign (never erases NVRAM)\r\n"
		     "exit            close the console\r\n"
		     "numbers are hex\r\n");
	} else if (!__builtin_strcmp(s, "md")) {
		a = hexarg(&p, &ok);
		v = *p ? hexarg(&p, &ok) : 0x40;
		if (ok)
			cmd_md(a, v > 0x10000 ? 0x10000 : v);
		else
			outs("? md ADDR [LEN]\r\n");
	} else if (!__builtin_strcmp(s, "mw")) {
		a = hexarg(&p, &ok);
		v = hexarg(&p, &ok);
		if (!ok || a & 3 || a + 4 > RAM_TOP || a + 4 < a) {
			outs("? mw ADDR WORD: a word-aligned RAM address\r\n");
		} else {
			*(volatile uint32_t *)a = v;
			cmd_md(a, 4);
		}
	} else if (!__builtin_strcmp(s, "info")) {
		cmd_info();
	} else if (!__builtin_strcmp(s, "boots")) {
		cmd_boots();
	} else if (!__builtin_strcmp(s, "nv")) {
		a = hexarg(&p, &ok);
		v = hexarg(&p, &ok);
		if (!ok || a < 0x7C00 || a > 0x7FFF || v > 0xFF) {
			outs("? nv OFF BYTE: an offset 7c00..7fff (the firmware's settings are below)\r\n");
		} else {
			*(volatile uint8_t *)(*(volatile uint32_t *)0x24 + a) = (uint8_t)v;
			cmd_md(*(volatile uint32_t *)0x24 + a, 1);
		}
	} else if (!__builtin_strcmp(s, "reset")) {
		outs("resetting\r\n");
		flush();
		hw_reset();
	} else if (!__builtin_strcmp(s, "apps")) {
		cmd_apps();
	} else if (!__builtin_strcmp(s, "time")) {
		a = hexarg(&p, &ok);
		if (ok) {
			apps_set_time(a);
			outs("time set\r\n");
		} else
			outs("? time SECS\r\n");
	} else if (!__builtin_strcmp(s, "adata")) {
		uint8_t buf[28];
		a = hexarg(&p, &ok);
		char *key = p;
		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			*p++ = 0;
		int n = ok && *key ? hexbytes(p, buf, sizeof buf) : -1;
		if (n < 0 || !apps_data(a, key, buf, (unsigned)n))
			outs("? adata LINE KEY HEX (an app on LINE, at most 28 bytes)\r\n");
		else
			outs("ok\r\n");
	} else if (!__builtin_strcmp(s, "sbegin")) {
		store_begin();
		outs("ok\r\n");
	} else if (!__builtin_strcmp(s, "sfile")) {
		char *name = p;
		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			*p++ = 0;
		a = hexarg(&p, &ok);
		const char *err = ok ? store_file(name, a) : "? sfile NAME SIZE";
		outs(err ? err : "ok");
		outs("\r\n");
	} else if (!__builtin_strcmp(s, "sd")) {
		uint8_t buf[32];
		int n = hexbytes(p, buf, sizeof buf);
		const char *err = n < 0 ? "? sd HEX" : store_data(buf, (uint32_t)n);
		outs(err ? err : "ok");
		outs("\r\n");
	} else if (!__builtin_strcmp(s, "sraw")) {
		a = hexarg(&p, &ok);
		if (!ok || !a)
			outs("? sraw SIZE, then SIZE bytes\r\n");
		else
			C.raw = a, C.rn = 0, C.rerr = 0, C.rdest = 0;
	} else if (!__builtin_strcmp(s, "ubegin")) {           /* update.c: a firmware update, shown on the panel */
		a = hexarg(&p, &ok);
		v = hexarg(&p, &ok);
		const char *err = ok ? update_begin(a, v) : "? ubegin SIZE CRC";
		outs(err ? err : "ok");
		outs("\r\n");
	} else if (!__builtin_strcmp(s, "uraw")) {
		a = hexarg(&p, &ok);
		if (!ok || !a)
			outs("? uraw SIZE, then SIZE bytes\r\n");
		else
			C.raw = a, C.rn = 0, C.rerr = 0, C.rdest = 1;
	} else if (!__builtin_strcmp(s, "ucommit")) {
		const char *err = update_commit();
		if (err) {
			outs(err);
			outs("\r\n");
		} else {                                         /* written: restart into it, as "reset" does */
			outs("updated: restarting\r\n");
			flush();
			hw_reset();
		}
	} else if (!__builtin_strcmp(s, "uabort")) {
		update_abort();
		outs("ok\r\n");
	} else if (!__builtin_strcmp(s, "scommit")) {
		const char *err = store_commit();
		if (!err)
			emoji_reset();                           /* native_emoji.c: a new emoji.bin is read on next use */
		outs(err ? err : "committed");
		outs("\r\n");
	} else if (!__builtin_strcmp(s, "sls")) {
		store_list(list_one);
		outs("end\r\n");
	} else if (!__builtin_strcmp(s, "exit")) {
		outs("bye\r\n");
		C.open = 0;
	} else {
		outs("? unknown command (help)\r\n");
	}
}

static void prompt(void)
{
	outs("mq> ");
	flush();
}

/* One received character while the console is open. */
static __attribute__((noinline)) void key(int c)
{
	if (c == '\r' || c == '\n') {
		if (c == '\n' && !C.len)
			return;
		C.line[C.len] = 0;
		outs("\r\n");
		exec(C.line);
		C.len = 0;
		if (C.open && !C.raw)                        /* after "sraw", the prompt waits for the bytes */
			prompt();
		else
			flush();
	} else if ((c == 8 || c == 127) && C.len) {
		C.len--;
		outs("\b \b");
		flush();
	} else if (c == 3) {
		C.len = 0;
		outs("^C\r\n");
		prompt();
	} else if (c >= 0x20 && c < 0x7F && C.len < LINE - 1) {
		C.line[C.len++] = (char)c;
		out((char)c);
		flush();
	}
}

int console_is_open(void) { return (int)C.open; }

/* One byte from the DB9. */
void console_rx(int c)
{
	if (!C.open) {
		C.magic = (uint8_t)c == (uint8_t)MAGIC[C.magic] ? C.magic + 1 : (uint8_t)c == (uint8_t)MAGIC[0];
		if (MAGIC[C.magic])
			return;
		C.magic = 0;
		C.bufs = g_outbuf;
		C.open = 1;
		C.len = C.used = C.raw = 0;
		outs("\r\nMQMON 1 marquee-sign console (help)\r\n");
		prompt();
		return;
	}
	if (C.raw) {                                 /* "sraw N": N bytes straight to the store's current file */
		C.rbuf[C.rn++] = (uint8_t)c;
		C.raw--;
		if (C.rn == sizeof C.rbuf || !C.raw) {
			if (C.rdest ? update_data(C.rbuf, C.rn) : !!store_data(C.rbuf, C.rn))
				C.rerr = 1;
			C.rn = 0;
		}
		if (!C.raw) {
			outs(C.rerr ? "data refused\r\n" : "ok\r\n");
			prompt();
		}
		return;
	}
	key(c);
}
