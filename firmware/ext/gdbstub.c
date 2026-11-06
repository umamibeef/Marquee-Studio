/* A minimal GDB remote-serial stub over the DB9 (SCC1), for debugging the from-source firmware on the real sign.
 *
 * start.S's exception path saves every register into crash_regs[] and (when the stub is armed) branches to
 * gdb_entry() instead of crash_report(). The stub speaks the GDB remote protocol until `c`/`s`, then calls
 * gdb_rfi() (start.S) to reload crash_regs and rfi back into the interrupted code. gdb_breakpoint() executes a
 * `trap`, so the firmware can halt for GDB at a chosen point (e.g. early in app_main, once SCC1 is up).
 *
 * Register block is the classic 32-bit PowerPC gdb order: r0..r31, f0..f31 (reported 0), pc, msr, cr, lr, ctr,
 * xer, fpscr. Attach with:  powerpc-eabi-gdb app.elf ; target remote :<port>  (through tools/gdb_bridge.py).
 */
#include <stdint.h>

extern uint32_t crash_regs[38];
enum { C_LR = 32, C_CR, C_CTR, C_XER, C_SRR0, C_SRR1 };

int  serial_read(char *dst, int max);
void serial_write(const char *s, int n);
void wdt_kick(void);
void gdb_rfi(void);                 /* start.S: reload crash_regs and rfi */

#define MSR_SE 0x00000400u          /* single-step enable */

static int getc_(void)
{
	char c;
	while (serial_read(&c, 1) != 1)
		wdt_kick();
	return (uint8_t)c;
}
static void putc_(int c) { char b = (char)c; serial_write(&b, 1); }

static const char HEX[] = "0123456789abcdef";
static int unhex(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static char pkt[1200];

/* Receive one packet body into pkt (NUL-terminated). Returns length, or -1 on Ctrl-C (0x03). */
static int rx_packet(void)
{
	for (;;) {
		int c = getc_();
		if (c == 0x03)
			return -1;
		if (c != '$')
			continue;
		unsigned n = 0, sum = 0;
		for (;;) {
			c = getc_();
			if (c == '#')
				break;
			if (n < sizeof pkt - 1)
				pkt[n++] = (char)c;
			sum = (sum + (unsigned)c) & 0xFF;
		}
		pkt[n] = 0;
		int h = getc_(), l = getc_();
		if (((unsigned)((unhex(h) << 4) | unhex(l)) & 0xFF) == sum) {
			putc_('+');
			return (int)n;
		}
		putc_('-');
	}
}

static void tx_packet(const char *s)
{
	for (;;) {
		unsigned sum = 0;
		putc_('$');
		for (const char *p = s; *p; p++) {
			putc_(*p);
			sum = (sum + (uint8_t)*p) & 0xFF;
		}
		putc_('#');
		putc_(HEX[(sum >> 4) & 0xF]);
		putc_(HEX[sum & 0xF]);
		int a = getc_();
		if (a == '+' || a == 0x03)
			return;
	}
}

static char out[1200];
static char *put_byte(char *p, uint8_t b) { *p++ = HEX[b >> 4]; *p++ = HEX[b & 0xF]; return p; }
static char *put_w(char *p, uint32_t v)    /* big-endian word */
{
	p = put_byte(p, (uint8_t)(v >> 24));
	p = put_byte(p, (uint8_t)(v >> 16));
	p = put_byte(p, (uint8_t)(v >> 8));
	return put_byte(p, (uint8_t)v);
}
static uint32_t get_hex(const char **s)
{
	uint32_t v = 0; int d;
	while ((d = unhex(**s)) >= 0) { v = (v << 4) | (uint32_t)d; (*s)++; }
	return v;
}
static uint32_t get_w(const char **s)      /* 8 fixed hex chars -> big-endian word */
{
	uint32_t v = 0;
	for (int i = 0; i < 8; i++) v = (v << 4) | (uint32_t)unhex(*(*s)++);
	return v;
}

#define NBP 24
static struct { uint32_t addr, orig; int used; } bp[NBP];

static void flush_i(uint32_t a)
{
	__asm__ volatile("dcbst 0,%0; sync; icbi 0,%0; isync" : : "r"(a) : "memory");
}
static void bp_set(const char *s)          /* "<addr>,<kind>" */
{
	uint32_t a = get_hex(&s);
	for (int i = 0; i < NBP; i++)
		if (bp[i].used && bp[i].addr == a) { tx_packet("OK"); return; }
	for (int i = 0; i < NBP; i++)
		if (!bp[i].used) {
			bp[i].addr = a;
			bp[i].orig = *(volatile uint32_t *)a;
			*(volatile uint32_t *)a = 0x7FE00008u;   /* trap */
			flush_i(a);
			bp[i].used = 1;
			tx_packet("OK");
			return;
		}
	tx_packet("E01");
}
static void bp_clr(const char *s)
{
	uint32_t a = get_hex(&s);
	for (int i = 0; i < NBP; i++)
		if (bp[i].used && bp[i].addr == a) {
			*(volatile uint32_t *)a = bp[i].orig;
			flush_i(a);
			bp[i].used = 0;
		}
	tx_packet("OK");
}

static void mem_read(const char *s)
{
	uint32_t a = get_hex(&s); if (*s == ',') s++;
	uint32_t n = get_hex(&s);
	char *p = out;
	for (uint32_t i = 0; i < n && (unsigned)(p - out) < sizeof out - 4; i++)
		p = put_byte(p, *(volatile uint8_t *)(a + i));
	*p = 0;
	tx_packet(out);
}
static void mem_write(const char *s)
{
	uint32_t a = get_hex(&s); if (*s == ',') s++;
	uint32_t n = get_hex(&s); if (*s == ':') s++;
	for (uint32_t i = 0; i < n; i++) {
		*(volatile uint8_t *)(a + i) = (uint8_t)((unhex(*s) << 4) | unhex(s[1]));
		s += 2;
	}
	tx_packet("OK");
}

static void regs_read(void)
{
	char *p = out;
	for (int i = 0; i < 32; i++) p = put_w(p, crash_regs[i]);
	for (int i = 0; i < 32 * 16; i++) *p++ = '0';            /* f0..f31 = 0 */
	p = put_w(p, crash_regs[C_SRR0]);                        /* pc  */
	p = put_w(p, crash_regs[C_SRR1]);                        /* msr */
	p = put_w(p, crash_regs[C_CR]);
	p = put_w(p, crash_regs[C_LR]);
	p = put_w(p, crash_regs[C_CTR]);
	p = put_w(p, crash_regs[C_XER]);
	p = put_w(p, 0);                                         /* fpscr */
	*p = 0;
	tx_packet(out);
}
static void regs_write(const char *s)
{
	for (int i = 0; i < 32; i++) crash_regs[i] = get_w(&s);
	for (int i = 0; i < 32 * 16; i++) s++;                   /* skip f0..f31 */
	crash_regs[C_SRR0] = get_w(&s);
	crash_regs[C_SRR1] = get_w(&s);
	crash_regs[C_CR]   = get_w(&s);
	crash_regs[C_LR]   = get_w(&s);
	crash_regs[C_CTR]  = get_w(&s);
	crash_regs[C_XER]  = get_w(&s);
	tx_packet("OK");
}

static int streq(const char *a, const char *b) { while (*b) if (*a++ != *b++) return 0; return 1; }

static int resumed;

void gdb_entry(void)
{
	serial_write("MQGDB2\r\n", 8);
	if (resumed) { resumed = 0; tx_packet("S05"); }
	for (;;) {
		int n = rx_packet();
		if (n < 0) { tx_packet("S05"); continue; }
		switch (pkt[0]) {
		case '?': tx_packet("S05"); break;
		case 'g': regs_read(); break;
		case 'G': regs_write(pkt + 1); break;
		case 'm': mem_read(pkt + 1); break;
		case 'M': mem_write(pkt + 1); break;
		case 'c': resumed = 1; gdb_rfi(); break;                 /* no return */
		case 's': crash_regs[C_SRR1] |= MSR_SE; resumed = 1; gdb_rfi(); break;
		case 'Z': if (pkt[1] == '0') bp_set(pkt + 3); else tx_packet(""); break;
		case 'z': if (pkt[1] == '0') bp_clr(pkt + 3); else tx_packet(""); break;
		case 'H': tx_packet("OK"); break;
		case 'q':
			if (streq(pkt, "qSupported")) tx_packet("PacketSize=4000");
			else if (streq(pkt, "qC")) tx_packet("QC01");
			else if (streq(pkt, "qAttached")) tx_packet("1");
			else if (streq(pkt, "qfThreadInfo")) tx_packet("m01");
			else if (streq(pkt, "qsThreadInfo")) tx_packet("l");
			else tx_packet("");
			break;
		default: tx_packet(""); break;
		}
	}
}

/* Halt here for GDB (executes `trap` -> program exception -> gdb_entry). */
void gdb_breakpoint(void) { serial_write("MQGDB1\r\n", 8); __asm__ volatile("trap"); }
