/* CPM SCC UART driver. SCC1 is the DB9 on this board (PA14/PA15, BRG2); SCC2 is the RS-485 port (PA12/PA13, BRG1, direction
 * on PB18). Polled receive through a ring of descriptors, closed by the idle timeout so short lines arrive at once. */
#include "serial.h"
#include "serial2.h"
#include "hw.h"

struct sbd { volatile uint16_t status, length; volatile uint32_t buf; };
#define BD_READY 0x8000u
#define BD_WRAP  0x2000u
#define BD_INT   0x1000u
#define NRX      8
#define RXSZ     64

struct chan {
	uint32_t regs, pram;      /* GSMR block and parameter RAM, from IMMR */
	uint32_t txbd, rxbd, rxbuf;
	uint16_t init;            /* CP command, without FLG */
	int next;
	char tx[256] __attribute__((aligned(16)));
};
static struct chan C[2] = {
	{ 0xA00, 0x3C00, 0x2820, 0x2830, 0x2900, 0x0000, 0, {0} },
	{ 0xA20, 0x3D00, 0x2870, 0x2880, 0x2B00, 0x0040, 0, {0} },
};
#define BD(off) ((struct sbd *)(IMMR + (off)))

static uint32_t brgc(uint32_t baud)
{
	uint32_t cd = brg_clock_hz() / (baud * 16u) - 1u;
	return cd < 0x1000u ? (cd << 1) | 0x10000u : ((brg_clock_hz() / (baud * 256u) - 1u) << 1) | 0x10001u;
}

static void chan_init(struct chan *c)
{
	uint32_t r = c->regs, p = c->pram;
	HW32(r + 0x00) = 0;
	struct sbd *t = BD(c->txbd);
	t->status = BD_WRAP;
	t->length = 0;
	t->buf = (uint32_t)c->tx;
	for (int i = 0; i < NRX; i++) {
		struct sbd *b = BD(c->rxbd + 8u * (uint32_t)i);
		b->length = 0;
		b->buf = IMMR + c->rxbuf + RXSZ * (uint32_t)i;
		b->status = (uint16_t)(BD_READY | (i == NRX - 1 ? BD_WRAP : 0));
	}
	c->next = 0;
	HW16(p + 0x00) = (uint16_t)c->rxbd;
	HW16(p + 0x02) = (uint16_t)c->txbd;
	HW8(p + 0x04) = 0x18;
	HW8(p + 0x05) = 0x18;
	HW16(p + 0x06) = RXSZ;
	cpm_command((uint16_t)(c->init | 1u));
	HW16(p + 0x38) = 2;       /* MAX_IDL: close a part-filled buffer after two idle characters */
	HW16(p + 0x3C) = 0;
	HW16(p + 0x3E) = 0; HW16(p + 0x40) = 0; HW16(p + 0x42) = 0; HW16(p + 0x44) = 0;
	HW16(p + 0x48) = 0; HW16(p + 0x4A) = 0;
	HW16(p + 0x4E) = 0;
	HW16(p + 0x50) = 0x8000;
	HW16(p + 0x60) = 0;
	HW16(r + 0x10) = 0xFFFF;
	HW16(r + 0x14) = 0;
	HW32(r + 0x04) = 0x00000060u;
	HW32(r + 0x00) = 0x00028004u;
	HW16(r + 0x08) = 0x3000;
	HW32(r + 0x00) = 0x00028034u;
}

static void chan_write(struct chan *c, const char *s, int n)
{
	struct sbd *t = BD(c->txbd);
	while (n > 0) {
		while (t->status & BD_READY)
			wdt_kick();
		int k = n > (int)sizeof c->tx ? (int)sizeof c->tx : n;
		for (int i = 0; i < k; i++)
			c->tx[i] = s[i];
		cache_flush(c->tx, (uint32_t)k);
		t->length = (uint16_t)k;
		t->buf = (uint32_t)c->tx;
		t->status = BD_READY | BD_WRAP | BD_INT;
		s += k;
		n -= k;
	}
}

static int chan_read(struct chan *c, char *dst, int max)
{
	int got = 0;
	while (got + RXSZ <= max) {
		struct sbd *b = BD(c->rxbd + 8u * (uint32_t)c->next);
		uint16_t st = b->status;
		if (st & BD_READY)
			break;
		const volatile uint8_t *src = (const volatile uint8_t *)b->buf;
		int n = b->length;
		for (int i = 0; i < n; i++)
			dst[got++] = (char)src[i];
		b->length = 0;
		b->status = (uint16_t)(BD_READY | (st & BD_WRAP));
		c->next = (c->next + 1) % NRX;
	}
	return got;
}

static void puts_(struct chan *c, const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	chan_write(c, s, n);
}

/* ---- SCC1: the DB9 ---- */
void serial_init(uint32_t baud)
{
	HW32(0xA00) = 0;
	HW16(0x952) |= 0x0003;
	HW16(0x950) &= (uint16_t)~0x0003;
	HW32(0xAEC) = (HW32(0xAEC) & ~0x000000FFu) | 0x09u;
	HW32(0x9F4) = brgc(baud);
	chan_init(&C[0]);
}
void serial_write(const char *s, int n) { chan_write(&C[0], s, n); }
void serial_puts(const char *s) { puts_(&C[0], s); }
int  serial_read(char *dst, int max) { return chan_read(&C[0], dst, max); }

/* ---- SCC2: RS-485 ---- */
static uint32_t scc_baud = 19200;       /* the line's baud, for the transmit-drain delay */
void scc_init(uint32_t baud)
{
	HW32(0xA20) = 0;
	HW16(0x952) |= 0x000C;
	HW16(0x950) &= (uint16_t)~0x000C;
	scc_baud = baud ? baud : 19200u;
	/* PB18 is the RS-485 transceiver's driver-enable: a push-pull GPIO output, idle low (receive). scc_write
	 * raises it around a transmission and drops it once the last character has shifted out (0x2000 = PB18 in the
	 * low half of each Port B register: PBPAR 0xABE, PBODR 0xAC2, PBDAT 0xAC6, PBDIR 0xABA). */
	HW16(0xABE) &= (uint16_t)~0x2000u;   /* PBPAR: general-purpose I/O, not a peripheral pin */
	HW16(0xAC2) &= (uint16_t)~0x2000u;   /* PBODR: push-pull */
	HW16(0xAC6) &= (uint16_t)~0x2000u;   /* PBDAT: idle low -> receive */
	HW16(0xABA) |= 0x2000u;              /* PBDIR: output */
	HW32(0xAEC) &= ~0x0000FF00u;
	HW32(0x9F0) = brgc(baud);
	chan_init(&C[1]);
}
/* RS-485 is half-duplex: raise the driver-enable (PB18) before sending, and lower it only after the CPM has sent the
 * buffer AND the last character has shifted out, or the bus contends with the next talker. The shift register has no
 * status bit in this polled driver, so after the buffer-descriptor is consumed we wait ~2 character times. The sim's
 * loopback PTY ignores PB18, so this is a no-op there. (DE assumed active-high; validate polarity/timing on silicon.) */
static void scc_tx_drain(void)
{
	struct sbd *t = BD(C[1].txbd);
	while (t->status & BD_READY)
		wdt_kick();
	delay_us(20000000u / scc_baud);
}
void scc_write(const char *s, int n)
{
	HW16(0xAC6) |= 0x2000u;              /* PB18 high: enable the line driver (transmit) */
	chan_write(&C[1], s, n);
	scc_tx_drain();
	HW16(0xAC6) &= (uint16_t)~0x2000u;   /* PB18 low: release the bus (receive) */
}
void scc_puts(const char *s) { puts_(&C[1], s); }
int  scc_read(char *dst, int max) { return chan_read(&C[1], dst, max); }
