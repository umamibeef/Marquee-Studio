/* Crash report: when the CPU takes an exception, the DB9 gets a report, then the watchdog restarts the sign.
 *
 * start.S saves every register into crash_regs and calls crash_report with the vector. The report goes straight into
 * the DB9 UART's (SCC1) TX descriptor at the CPM's own pointer, polling its ready bit, so it needs nothing else alive.
 * It runs in real mode: only RAM in bank 0, where the image lives, is safe to read, and the words of the stack walk
 * are range-checked.
 *
 *   MQCRASH 1 UM0.1
 *   vector 0700 pc 0004a2c0 msr 00009032 lr 00012340 dar 00000000 dsisr 00000000
 *   r00 ... (8 per line, 4 lines)
 *   cr ... ctr ... xer ...
 *   stack: sp/lr pairs, up to 12 frames
 *   MQCRASH END
 */
#include <stdint.h>
#include "../../bootloader/include/dbg.h"

void fw_crash_record(uint32_t vector, uint32_t srr0, uint32_t srr1, uint32_t dar, uint32_t dsisr, uint32_t lr,
                     uint32_t cr, const uint32_t *r);

#define IMMR     0xFF000000u
#define RAM_TOP  (*(volatile uint32_t *)0x14)
#ifndef FW_VERSION
#define FW_VERSION "?"
#endif

/* start.S: r0..r31, then lr, cr, ctr, xer, srr0, srr1 */
uint32_t crash_regs[38];
enum { C_LR = 32, C_CR, C_CTR, C_XER, C_SRR0, C_SRR1 };

#define OUT_SIZE 128
static char OUT_BUF[OUT_SIZE] __attribute__((aligned(16)));
struct out { char *buf; unsigned n; };

static __attribute__((noinline)) void send(struct out *o)
{
	volatile uint16_t *tbptr;
	volatile uint8_t *bd;
	uint32_t a, spin;

	if (!o->n)
		return;
	for (a = (uint32_t)o->buf & ~15u; a < (uint32_t)o->buf + o->n; a += 16)
		__asm__ volatile("dcbst 0, %0" :: "r"(a) : "memory");
	__asm__ volatile("sync" ::: "memory");
	tbptr = (volatile uint16_t *)(IMMR + 0x3C00u + 0x20);
	/* TBPTR holds the TX BD offset relative to IMMR (serial.c programs TBASE = txbd = 0x2820,
	 * and BD(off) = IMMR + off); do NOT add 0x2000 again or the address lands past the DPRAM. */
	bd = (volatile uint8_t *)(IMMR + *tbptr);
	for (spin = 0; (bd[0] & 0x80) && spin < 2000000; spin++)     /* the CPM still sending the BD before */
		;
	*(volatile uint32_t *)(bd + 4) = (uint32_t)o->buf;
	*(volatile uint16_t *)(bd + 2) = (uint16_t)o->n;
	__asm__ volatile("sync" ::: "memory");
	*(volatile uint16_t *)bd = (uint16_t)((*(volatile uint16_t *)bd & 0x2000) | 0x8000 | 0x0800);   /* R, keep W, CM */
	for (spin = 0; (bd[0] & 0x80) && spin < 2000000; spin++)     /* wait until it is out */
		;
	o->n = 0;
}

static __attribute__((noinline)) void put(struct out *o, char c)
{
	o->buf[o->n++] = c;
	if (c == '\n' || o->n == OUT_SIZE)
		send(o);
}

static __attribute__((noinline)) void puts_(struct out *o, const char *s)
{
	while (*s)
		put(o, *s++);
}

static __attribute__((noinline)) void hex(struct out *o, uint32_t v, int digits)
{
	while (digits--)
		put(o, "0123456789abcdef"[v >> (4 * digits) & 15]);
}

static __attribute__((noinline)) void field(struct out *o, const char *name, uint32_t v)
{
	puts_(o, name);
	put(o, ' ');
	hex(o, v, 8);
	put(o, ' ');
}

static __attribute__((noinline)) void nl(struct out *o)
{
	put(o, '\r');
	put(o, '\n');
}

static int ram_word(uint32_t a)
{
	return !(a & 3) && a >= 0x100 && a + 4 <= RAM_TOP;
}

void crash_report(uint32_t vector)
{
	struct out o;
	const uint32_t *r = crash_regs;
	uint32_t dar, dsisr, mdepn, sp, i;

	__asm__ volatile("mfspr %0, 19" : "=r"(dar));
	__asm__ volatile("mfspr %0, 18" : "=r"(dsisr));
	__asm__ volatile("mfspr %0, 795" : "=r"(mdepn));                  /* the address of a TLB miss */
	fw_crash_record(vector, r[C_SRR0], r[C_SRR1], dar, dsisr, r[C_LR], r[C_CR], r);
	o.buf = OUT_BUF;
	o.n = 0;

	puts_(&o, "\r\nMQCRASH 1 ");
	puts_(&o, FW_VERSION);
	nl(&o);
	field(&o, "vector", vector);
	field(&o, "pc", r[C_SRR0]);
	field(&o, "srr1", r[C_SRR1]);
	field(&o, "lr", r[C_LR]);
	field(&o, "dar", dar);
	field(&o, "dsisr", dsisr);
	field(&o, "epn", mdepn);
	nl(&o);
	for (i = 0; i < 32; i++) {
		put(&o, 'r');
		put(&o, (char)('0' + i / 10));
		put(&o, (char)('0' + i % 10));
		put(&o, ' ');
		hex(&o, r[i], 8);
		put(&o, ' ');
		if ((i & 7) == 7)
			nl(&o);
	}
	field(&o, "cr", r[C_CR]);
	field(&o, "ctr", r[C_CTR]);
	field(&o, "xer", r[C_XER]);
	nl(&o);
	puts_(&o, "stack");
	nl(&o);
	for (sp = r[1], i = 0; i < 12 && ram_word(sp) && ram_word(sp + 4); i++) {
		uint32_t next = *(uint32_t *)sp;
		if (!ram_word(next) || !ram_word(next + 4) || next <= sp)
			break;
		field(&o, "sp", next);
		field(&o, "lr", *(uint32_t *)(next + 4));
		nl(&o);
		sp = next;
	}
	puts_(&o, "MQCRASH END");
	nl(&o);
	send(&o);
	for (;;)
		;                                                        /* the watchdog restarts the sign */
}
