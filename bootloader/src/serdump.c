/* Serial dump window for BOOTSTRAP V2.00 (C rebuild), built with `make DUMP=1`.
 *
 * Runs where the network window does: SDRAM is up, the caches are off and the app is not
 * loaded yet. It
 *  1. sets up the DB9 UART the way the app does (SMC1, or SCC1 on boards with the LED-panel
 *     lines off Port D), polled, 115200 8N1;
 *  2. listens DUMP_LISTEN_MS for DUMP_MAGIC. Without it, it puts back every register it
 *     changed and returns, and the boot goes on as before;
 *  3. with it, serves the frames of include/serdump.h until a BOOT request or DUMP_IDLE_MS
 *     without one, then puts everything back and returns.
 *
 * Read-only by construction. The only stores are:
 *  - DPRAM scratch (0xFF002C00..0xFF002FFF) and the stack (DPRAM, below 0xFF002C00);
 *  - the UART, BRG2, port, routing and timer 2 registers, put back on the way out;
 *  - the I2C buffer descriptors of a normal EEPROM read (i2c_read, as at boot).
 * There is no flash command, no NVRAM or EEPROM write, and no way to start other code: the
 * window only returns to the normal boot. Reads go through region() and nowhere else.
 * SDRAM is not written either, so a warm-reset SDRAM dump shows what the last run left. */
#include "board.h"
#include "serdump.h"
#include "archive.h"

/* ---- CPM buffer descriptors (UART mode) ---- */
struct bd {
	volatile uint16_t status;
	volatile uint16_t len;
	volatile uint32_t buf;
};

#define RXBD      ((struct bd *)DUMP_RXBD)
#define TXBD      ((struct bd *)DUMP_TXBD)
#define BD_E      0x8000      /* RX: empty (owned by the CPM) */
#define BD_R      0x8000      /* TX: ready (owned by the CPM) */
#define BD_W      0x2000      /* wrap: last BD in the ring */
#define BD_RX_ERR 0x003B      /* BR | FR | PR | OV | CD */

#define P8(b, o)  (*(volatile uint8_t *)((b) + (o)))
#define P16(b, o) (*(volatile uint16_t *)((b) + (o)))
#define D8(a)     (*(volatile uint8_t *)(a))

#define TX_TIMEOUT_MS  500    /* one BD (at most 512 bytes) must go out within this */

struct dump {
	int      scc;                      /* 1: SCC1 (Port C/B boards), 0: SMC1 */
	uint32_t gclk, brgclk;             /* as the app computes them */
	uint32_t n0, n;                    /* BRG divider: at DUMP_BAUD, now */
	uint32_t ticks_per_ms, acc;
	uint16_t last_tcn;
	unsigned rx_next, tx_next;
	unsigned bounce, bounce_bd[DUMP_BOUNCE_N];  /* next bounce buffer; TX BD + 1 using each */
	uint8_t  ring[64];                 /* received characters, not yet parsed */
	unsigned head, tail;
	uint8_t  rx[DUMP_HDR + DUMP_FIXED_MAX + 4];   /* request being assembled (WRITE data goes to the stage) */
	unsigned rx_n;                     /* payload length of the request being assembled */
	uint32_t rx_crc;                   /* its CRC so far */
	uintptr_t wr_addr;                 /* WRITE: where its data goes in the stage, 0 = nowhere */
	unsigned rx_len, req_len;
	unsigned text_len;
	uint32_t rx_errors;
	int      boot, dead;
	/* registers to put back */
	uint16_t s_papar, s_padir, s_smcmr, s_psmr, s_tgcr, s_tmr2, s_trr2, s_tcn2;
	uint32_t s_pbpar, s_pbdir, s_simode, s_sicr, s_brgc2, s_gsmrl, s_gsmrh;
};

/* ---- CRC-32 (zlib: reflected 0xEDB88320), table from the usual generator ---- */
static const uint32_t crc_table[256] = {
	0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
	0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
	0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
	0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
	0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
	0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
	0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
	0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
	0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
	0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
	0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
	0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
	0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
	0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
	0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
	0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
	0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
	0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
	0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
	0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
	0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
	0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
	0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
	0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
	0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
	0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
	0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
	0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
	0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
	0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
	0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
	0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
	0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
	0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
	0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
	0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
	0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
	0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
	0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
	0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
	0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693,
	0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
	0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

static uint32_t crc32_update(uint32_t crc, uintptr_t a, uint32_t n)
{
	while (n--)
		crc = crc_table[(crc ^ D8(a++)) & 0xFF] ^ (crc >> 8);
	return crc;
}

/* ---- clock: CPM timer 2 free-running at gclk / 250, as in the network window ---- */
static void clock_start(struct dump *d)
{
	d->s_tgcr = TGCR;
	d->s_tmr2 = TMR2;
	d->s_trr2 = TRR2;
	d->s_tcn2 = TCN2;
	d->ticks_per_ms = d->gclk / 250 / 1000;
	if (!d->ticks_per_ms)
		d->ticks_per_ms = 1;
	TGCR &= (uint16_t)~0x00F0;          /* timer 2: stopped, not cascaded */
	TMR2 = 0xF902;                      /* prescale 250, system clock, free run */
	TRR2 = 0xFFFF;
	TCN2 = 0;
	TER2 = 0xFFFF;
	TGCR |= 0x0010;                     /* RST2: run */
	d->last_tcn = 0;
	d->acc = 0;
}

static void clock_stop(struct dump *d)
{
	TGCR = (uint16_t)((TGCR & ~0x00F0) | (d->s_tgcr & 0x00F0));
	TMR2 = d->s_tmr2;
	TRR2 = d->s_trr2;
	TCN2 = d->s_tcn2;
	TER2 = 0xFFFF;
}

/* Milliseconds since clock_start. TCN2 wraps every 327 ms at 50 MHz: call often. */
static uint32_t now_ms(struct dump *d)
{
	uint16_t t = TCN2;

	d->acc += (uint16_t)(t - d->last_tcn);
	d->last_tcn = t;
	return d->acc / d->ticks_per_ms;
}

static void icache(int on)
{
	if (on) {
		MTSPR(SPR_IC_CST, 0x0A000000); isync_sync();    /* invalidate */
		MTSPR(SPR_IC_CST, 0x0C000000); isync_sync();    /* unlock */
		MTSPR(SPR_IC_CST, 0x02000000); isync_sync();    /* enable */
	} else {
		MTSPR(SPR_IC_CST, 0x04000000); isync_sync();    /* disable, as at reset */
	}
}

/* ---- UART ----
 * Pins and clocks follow the app's serial driver: serial_pins_config (0x532D0) with its
 * tables at 0xB94A0 (default board) and 0xB9DE8 (Port C/B boards), smc_uart_init (0x53B80),
 * scc_uart_init (0x53528) and cpm_brg_value (0x533A0):
 *   default board, /dev/rs232 = SMC1: PB24 SMRXD1, PB25 SMTXD1; clock BRG2 (SIMODE SMC1CS 1)
 *   Port C/B boards, /dev/rs232 = SCC1: PA15 RXD1, PA14 TXD1; clock BRG2 (SICR R1CS = T1CS 1)
 * The app also makes the DB9 modem lines GPIO (default board: PD7 and PD10 outputs, PC6, PC7,
 * PC12 and PA3 inputs; its factory loopback test diag_pin_loopback_test drives them) and puts
 * PA5 in its dedicated-output mode. TXD and RXD do not need any of that, so those pins keep
 * their reset state here. Only one channel is set up: each one's pins have another use on the
 * other board (on Port C/B boards PB25 is a GPIO output). */

/* The app's board_panel_lines_off_portd (0x55ACC): /dev/rs232 is SCC1 on these boards. */
static int rs232_on_scc1(void)
{
	uint8_t t = cfg8(CFG_BUF, CFG_MEM_TYPE);

	return t == 2 || (t == 1 && cfg8(CFG_BUF, CFG_MEM_SUB) >= 0x50);
}

static void cpm_cmd(uint16_t op)
{
	uint32_t n = 100000;

	CPCR = op | 0x0001;                 /* FLG */
	while ((CPCR & 0x0001) && --n)
		;
}

/* BRG2 = BRGCLK / 16 / n: EN, CD = n - 1 (as cpm_brg_value; n <= 4096, no /16 prescaler) */
static void brg_set(struct dump *d, uint32_t n)
{
	BRGC(2) = (n - 1) << 1 | 0x00010000;
	d->n = n;
}

static uint32_t baud_of(struct dump *d, uint32_t n)
{
	return d->brgclk / 16 / n;
}

static void uart_start(struct dump *d)
{
	uintptr_t pram = d->scc ? SCC1_PRAM : SMC1_PRAM;
	unsigned i;

	d->s_papar = PAPAR;
	d->s_padir = PADIR;
	d->s_pbpar = PBPAR;
	d->s_pbdir = PBDIR;
	d->s_simode = SIMODE;
	d->s_sicr = SICR;
	d->s_brgc2 = BRGC(2);
	d->s_smcmr = SMCMR1;
	d->s_gsmrl = GSMRL1;
	d->s_gsmrh = GSMRH1;
	d->s_psmr = PSMR1;

	for (i = 0; i < DUMP_RX_RING; i++) {
		RXBD[i].buf = DUMP_RXBUF + i;
		RXBD[i].len = 0;
		RXBD[i].status = BD_E | (i == DUMP_RX_RING - 1 ? BD_W : 0);
	}
	for (i = 0; i < DUMP_TX_RING; i++) {
		TXBD[i].buf = 0;
		TXBD[i].len = 0;
		TXBD[i].status = i == DUMP_TX_RING - 1 ? BD_W : 0;
	}
	d->rx_next = 0;
	d->tx_next = 0;
	brg_set(d, d->n0);

	if (d->scc) {
		GSMRL1 = 0;                                 /* off while it is set up */
		PAPAR |= 0x0003;                            /* PA14 TXD1, PA15 RXD1 */
		PADIR &= (uint16_t)~0x0003;
		SICR = (SICR & ~0x000000FFu) | 0x09;        /* SCC1: NMSI, rx and tx clock BRG2 */
	} else {
		SMCMR1 = 0;
		PBPAR |= 0x000000C0;                        /* PB24 SMRXD1, PB25 SMTXD1 */
		PBDIR &= ~0x000000C0u;
		SIMODE = (SIMODE & ~0x0000F000u) | 0x00001000;  /* SMC1: NMSI, clock BRG2 */
	}

	/* RBASE/TBASE as the app writes them: the low half of the BD address */
	P16(pram, 0) = (uint16_t)DUMP_RXBD;
	P16(pram, 2) = (uint16_t)DUMP_TXBD;
	P8(pram, 4) = 0x18;                             /* RFCR, TFCR: big-endian */
	P8(pram, 5) = 0x18;
	P16(pram, 6) = 1;                               /* MRBLR: one character per BD */
	cpm_cmd(d->scc ? 0x0000 : 0x0090);              /* INIT RX AND TX: SCC1 / SMC1 */

	if (d->scc) {
		P16(pram, 0x38) = 0;                        /* MAX_IDL: off (one-byte buffers) */
		P16(pram, 0x3C) = 0;                        /* BRKCR */
		P16(pram, 0x3E) = 0;                        /* PAREC, FRMEC, NOSEC, BRKEC */
		P16(pram, 0x40) = 0;
		P16(pram, 0x42) = 0;
		P16(pram, 0x44) = 0;
		P16(pram, 0x48) = 0;                        /* UADDR1, UADDR2 */
		P16(pram, 0x4A) = 0;
		P16(pram, 0x4E) = 0;                        /* TOSEQ */
		P16(pram, 0x50) = 0x8000;                   /* CHARACTER1: end of the control table */
		P16(pram, 0x60) = 0;                        /* RCCM */
		SCCE1 = 0xFFFF;
		SCCM1 = 0;                                  /* polled: no interrupts */
		GSMRH1 = 0x00000060;                        /* as the app: RFW, TFL */
		GSMRL1 = 0x00028004;                        /* UART, 16x rx and tx clocks */
		PSMR1 = 0x3000;                             /* 8 data bits, 1 stop, no parity */
		GSMRL1 = 0x00028034;                        /* ENT | ENR */
	} else {
		P16(pram, 0x28) = 0;                        /* MAX_IDL: off (one-byte buffers) */
		P16(pram, 0x2C) = 0;                        /* BRKLN, BRKEC, BRKCR */
		P16(pram, 0x2E) = 0;
		P16(pram, 0x30) = 0;
		SMCE1 = 0xFF;
		SMCM1 = 0;                                  /* polled: no interrupts */
		SMCMR1 = 0x4820;                            /* UART, CLEN 9: 8 data bits, 1 stop */
		SMCMR1 = 0x4823;                            /* TEN | REN */
	}
}

static void uart_stop(struct dump *d)
{
	if (d->scc) {
		GSMRL1 = d->s_gsmrl;
		GSMRH1 = d->s_gsmrh;
		PSMR1 = d->s_psmr;
		SCCE1 = 0xFFFF;
		SICR = d->s_sicr;
		PAPAR = d->s_papar;
		PADIR = d->s_padir;
	} else {
		SMCMR1 = d->s_smcmr;
		SMCE1 = 0xFF;
		SIMODE = d->s_simode;
		PBPAR = d->s_pbpar;
		PBDIR = d->s_pbdir;
	}
	BRGC(2) = d->s_brgc2;
}

/* Move received characters from the RX BDs to the ring. Called from every wait loop. */
static void rx_poll(struct dump *d)
{
	struct bd *bd;

	while (!((bd = &RXBD[d->rx_next])->status & BD_E)) {
		uint16_t st = bd->status;
		unsigned next = (d->head + 1) % sizeof d->ring;

		if ((st & BD_RX_ERR) || bd->len != 1 || next == d->tail)
			d->rx_errors++;
		else {
			d->ring[d->head] = D8(bd->buf);
			d->head = next;
		}
		bd->len = 0;
		bd->status = BD_E | (d->rx_next == DUMP_RX_RING - 1 ? BD_W : 0);
		d->rx_next = (d->rx_next + 1) % DUMP_RX_RING;
	}
}

static int rx_pop(struct dump *d)
{
	int c;

	if (d->tail == d->head)
		return -1;
	c = d->ring[d->tail];
	d->tail = (d->tail + 1) % sizeof d->ring;
	return c;
}

/* Wait until TX BD i is no longer ready. Gives up (dead) if the UART stops taking data. */
static int tx_wait_bd(struct dump *d, unsigned i)
{
	uint32_t t = now_ms(d);

	while (TXBD[i].status & BD_R) {
		wdt_kick();
		rx_poll(d);
		if (now_ms(d) - t > TX_TIMEOUT_MS) {
			d->dead = 1;
			return -1;
		}
	}
	return 0;
}

static int tx_wait_idle(struct dump *d)
{
	unsigned i;

	for (i = 0; i < DUMP_TX_RING; i++)
		if (d->dead || tx_wait_bd(d, i))
			return -1;
	return 0;
}

static int tx_queue(struct dump *d, uintptr_t buf, uint32_t len)
{
	struct bd *bd = &TXBD[d->tx_next];

	if (d->dead || tx_wait_bd(d, d->tx_next))
		return -1;
	bd->buf = buf;
	bd->len = (uint16_t)len;
	bd->status = BD_R | (d->tx_next == DUMP_TX_RING - 1 ? BD_W : 0);
	d->tx_next = (d->tx_next + 1) % DUMP_TX_RING;
	return 0;
}

/* Busy-wait ms milliseconds, still taking in characters */
static void pause_ms(struct dump *d, uint32_t ms)
{
	uint32_t t = now_ms(d);

	while (now_ms(d) - t <= ms) {
		wdt_kick();
		rx_poll(d);
	}
}

/* Let the last characters leave the shift register (a few character times) */
static void tx_drain(struct dump *d)
{
	tx_wait_idle(d);
	pause_ms(d, 3);
}

/* ---- frames ---- */
static int in_dpram(uintptr_t a)
{
	return a >= DPRAM(0) && a < DPRAM(0x2000);
}

/* Send one frame: the header with the small payload p[0..n) in the header buffer, then len
 * bytes at data, then the CRC-32. Data in DPRAM goes out as it is; anything else (flash,
 * NVRAM, SDRAM) is copied through the bounce buffers, so the CPM only ever reads DPRAM. */
static int send_frame(struct dump *d, uint8_t type, uint8_t tag, const uint8_t *p, unsigned n,
                      uintptr_t data, uint32_t len)
{
	volatile uint8_t *h = (volatile uint8_t *)DUMP_HDRBUF;
	volatile uint8_t *t = (volatile uint8_t *)DUMP_TRLBUF;
	uint32_t crc, total = n + len, i;

	if (tx_wait_idle(d))                            /* header and trailer free again */
		return -1;
	h[0] = DUMP_SYNC0;
	h[1] = DUMP_SYNC1;
	h[2] = type;
	h[3] = tag;
	h[4] = (uint8_t)(total >> 8);
	h[5] = (uint8_t)total;
	for (i = 0; i < n; i++)
		h[DUMP_HDR + i] = p[i];
	crc = crc32_update(0xFFFFFFFF, DUMP_HDRBUF + 2, DUMP_HDR - 2 + n);
	if (tx_queue(d, DUMP_HDRBUF, DUMP_HDR + n))
		return -1;

	if (len && in_dpram(data)) {
		crc = crc32_update(crc, data, len);
		if (tx_queue(d, data, len))
			return -1;
	} else {
		while (len) {
			uint32_t k = len < DUMP_BOUNCE_SZ ? len : DUMP_BOUNCE_SZ;
			uintptr_t b = DUMP_BOUNCE + d->bounce * DUMP_BOUNCE_SZ;

			if (d->bounce_bd[d->bounce] && tx_wait_bd(d, d->bounce_bd[d->bounce] - 1))
				return -1;
			for (i = 0; i < k; i++)                 /* byte reads: 8/16-bit devices */
				D8(b + i) = D8(data + i);
			crc = crc32_update(crc, b, k);
			d->bounce_bd[d->bounce] = d->tx_next + 1;
			if (tx_queue(d, b, k))
				return -1;
			d->bounce = (d->bounce + 1) % DUMP_BOUNCE_N;
			data += k;
			len -= k;
		}
	}

	crc = ~crc;
	t[0] = (uint8_t)(crc >> 24);
	t[1] = (uint8_t)(crc >> 16);
	t[2] = (uint8_t)(crc >> 8);
	t[3] = (uint8_t)crc;
	return tx_queue(d, DUMP_TRLBUF, 4);
}

static void send_error(struct dump *d, uint8_t code, uint8_t type, uint8_t tag)
{
	uint8_t e[4] = { code, type, 0, 0 };

	send_frame(d, DUMP_ERROR, tag, e, 4, 0, 0);
}

/* DUMP_E_FLASH with where it failed: code, type, step, 0, address, word read, word expected (big-endian) */
static void send_flash_error(struct dump *d, uint8_t type, uint8_t tag)
{
	uint32_t a = flash_fail.addr;
	uint16_t g = flash_fail.got, w = flash_fail.want;
	uint8_t e[12] = { DUMP_E_FLASH, type, (uint8_t)flash_fail.step, 0,
			  (uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a,
			  (uint8_t)(g >> 8), (uint8_t)g, (uint8_t)(w >> 8), (uint8_t)w };

	send_frame(d, DUMP_ERROR, tag, e, 12, 0, 0);
}

/* Plain text (no frame): the READY and CLOSED lines */
static void send_line(struct dump *d, const char *s)
{
	volatile uint8_t *h = (volatile uint8_t *)DUMP_HDRBUF;
	unsigned n = 0;

	if (tx_wait_idle(d))
		return;
	while (s[n] && n < 32) {
		h[n] = (uint8_t)s[n];
		n++;
	}
	tx_queue(d, DUMP_HDRBUF, n);
}

static int region(unsigned id, uint32_t off, uint32_t len, uintptr_t *addr);
static uint32_t get32(const uint8_t *p);

static uint32_t get32x(const uint8_t *p, uint8_t tag)    /* a big-endian word of a request (XORed with tag) */
{
	return (uint32_t)(p[0] ^ tag) << 24 | (uint32_t)(p[1] ^ tag) << 16 | (uint32_t)(p[2] ^ tag) << 8 | (p[3] ^ tag);
}

/* Feed one character. Returns 1 when a whole request with a good CRC is in: its fixed fields in rx[] and, for
 * WRITE, its data already in place in the stage (stored as it arrives, so the window never needs SDRAM for a
 * buffer; a request with a bad CRC leaves those bytes wrong, and the host sends it again). */
static int rx_byte(struct dump *d, uint8_t c)
{
	unsigned n, i;
	uint32_t want;
	uint8_t tag;

	if (d->rx_len == 0 && c != DUMP_SYNC0)
		return 0;
	if (d->rx_len == 1 && c != DUMP_SYNC1) {
		d->rx_len = c == DUMP_SYNC0;
		return 0;
	}
	if (d->rx_len < DUMP_HDR) {
		d->rx[d->rx_len++] = c;
		if (d->rx_len < DUMP_HDR)
			return 0;
		n = (unsigned)d->rx[4] << 8 | d->rx[5];
		if (n > (d->rx[2] == DUMP_WRITE ? DUMP_REQ_MAX : DUMP_FIXED_MAX)) {
			d->rx_len = 0;                          /* not a request: hunt for the next sync */
			return 0;
		}
		d->rx_n = n;
		d->wr_addr = 0;
		d->rx_crc = crc32_update(0xFFFFFFFF, (uintptr_t)&d->rx[2], DUMP_HDR - 2);
		return 0;
	}
	n = d->rx_n;
	i = d->rx_len++ - DUMP_HDR;                     /* payload index, then the 4 CRC bytes */
	tag = d->rx[3];
	if (i < n) {
		d->rx_crc = crc_table[(d->rx_crc ^ c) & 0xFF] ^ (d->rx_crc >> 8);
		if (d->rx[2] == DUMP_WRITE && i >= 8) {
			if (i == 8) {                           /* region and offset are in: where does it go? */
				uintptr_t a;

				d->wr_addr = 0;
				if ((d->rx[DUMP_HDR] ^ tag) == DUMP_R_STAGE &&
				    region(DUMP_R_STAGE, get32x(&d->rx[DUMP_HDR + 4], tag), n - 8, &a) == 0)
					d->wr_addr = a;
			}
			if (d->wr_addr)
				*(volatile uint8_t *)(d->wr_addr + i - 8) = c ^ tag;
		} else {
			d->rx[DUMP_HDR + i] = c;
		}
		return 0;
	}
	d->rx[DUMP_HDR + DUMP_FIXED_MAX + (i - n)] = c;
	if (i - n < 3)
		return 0;
	d->rx_len = 0;
	want = get32(&d->rx[DUMP_HDR + DUMP_FIXED_MAX]);
	if (~d->rx_crc != want) {
		d->rx_errors++;
		return 0;
	}
	d->req_len = n;
	return 1;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* ---- the read allow-list ---- */
static int ee_read(uint8_t dev)
{
	int i;

	for (i = 0; i < 3; i++) {                       /* the boot's own random read from 0 */
		wdt_kick();
		if (i2c_read(dev, DUMP_EEBUF, 256) == 0)
			return 0;
	}
	return -1;
}

static void region_bounds(unsigned id, uintptr_t *base, uint32_t *size)
{
	switch (id) {
	case DUMP_R_BOOT:   *base = BI->boot_base;   *size = BI->boot_size;    break;
	case DUMP_R_FLASH1: *base = BI->flash1_base; *size = BI->flash1_total; break;
	case DUMP_R_FLASH2: *base = BI->flash2_base; *size = BI->flash2_total; break;
	case DUMP_R_NVRAM:  *base = BI->nvram_base;  *size = BI->nvram_size;   break;
	case DUMP_R_EE_A0:  *base = 0xA0;            *size = 256;              break;
	case DUMP_R_EE_A2:  *base = 0xA2;            *size = 256;              break;
	case DUMP_R_SDRAM:  *base = APP_LOAD;        *size = BI->bank_size;    break;  /* bank 0 */
	case DUMP_R_SLOT: {
		uint32_t s = slot_where(0);

		*base = s;
		*size = s ? SLOT_SIZE : 0;
		break;
	}
	case DUMP_R_STAGE: {
		uint32_t phys, virt;

		stage_where(&phys, &virt);
		*base = phys;
		*size = phys ? STAGE_SIZE : 0;
		break;
	}
	default:            *base = 0;               *size = 0;                break;
	}
}

/* The staging area: the last STAGE_SIZE bytes of the last SDRAM bank (physical), which is the end of the
 * app's RAM (virtual: the banks are mapped back to back from 0). 0 if there is no bank. */
void stage_where(uint32_t *phys, uint32_t *virt)
{
	const volatile uint32_t *b = (const volatile uint32_t *)BANK_LIST;
	uint32_t n = 0, last = 0, size = BI->bank_size;

	while (n < 16 && b[n] != 0xFFFFFFFF)
		last = b[n++];
	if (n == 0 || size < 2 * STAGE_SIZE) {
		*phys = *virt = 0;
		return;
	}
	*phys = last + size - STAGE_SIZE;
	*virt = n * size - STAGE_SIZE;
}

/* Check [off, off + len) against region id and give its address. EEPROMs are read live
 * into DUMP_EEBUF first. Returns 0 or a DUMP_E_ code. */
static int region(unsigned id, uint32_t off, uint32_t len, uintptr_t *addr)
{
	uintptr_t base;
	uint32_t size;

	region_bounds(id, &base, &size);
	if (size == 0)
		return DUMP_E_REGION;
	if (off >= size || len > size - off)
		return DUMP_E_RANGE;
	if (id == DUMP_R_EE_A0 || id == DUMP_R_EE_A2) {
		if (ee_read((uint8_t)base))
			return DUMP_E_I2C;
		base = DUMP_EEBUF;
	}
	*addr = base + off;
	return 0;
}

/* ---- info text (HELLO), built in the bounce area ---- */
static void put_s(struct dump *d, const char *s)
{
	while (*s && d->text_len < DUMP_TEXT_MAX)
		D8(DUMP_BOUNCE + d->text_len++) = (uint8_t)*s++;
}

static void put_x(struct dump *d, uint32_t v, int digits)
{
	char b[9];
	int i;

	for (i = 0; i < digits; i++)
		b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 0xF];
	b[digits] = 0;
	put_s(d, b);
}

static void put_u(struct dump *d, uint32_t v)
{
	char b[11];
	int i = 10;

	b[i] = 0;
	do
		b[--i] = (char)('0' + v % 10);
	while ((v /= 10) && i);
	put_s(d, &b[i]);
}

static void build_info(struct dump *d)
{
	static const char *const names[DUMP_R_COUNT] = {
		"", "bootflash", "flash1", "flash2", "nvram", "eeprom_a0", "eeprom_a2", "sdram", "stage", "slot"
	};
	unsigned id;

	d->text_len = 0;
	put_s(d, "MQDUMP 1 BOOTSTRAP V2.00\n");          /* HELLO fits DUMP_TEXT_MAX (512): keep it short */
	put_s(d, d->scc ? "uart=SCC1 board=portcb" : "uart=SMC1 board=default");
	put_s(d, " baud="); put_u(d, baud_of(d, d->n));
	put_s(d, "\nid="); put_x(d, cfg32(CFG_BUF, CFG_BOARD_ID), 8);
	put_s(d, " cfg="); put_x(d, cfg8(CFG_BUF, CFG_MEM_TYPE), 2); put_x(d, cfg8(CFG_BUF, CFG_MEM_SUB), 2);
	put_s(d, " opt="); put_x(d, cfg32(CFG_BUF, CFG_OPTIONS), 8);
	put_s(d, cfg8(CFG_BUF, CFG_MEM_TYPE) == 1 && cfg8(CFG_BUF, CFG_MEM_SUB) < 0x50 ?
	      " mem=spd" : " mem=fixed");
	put_s(d, "\nsysclk="); put_u(d, BI->sysclk);
	put_s(d, " sdram="); put_x(d, BI->sdram_total, 8);
	put_s(d, " bank="); put_x(d, BI->bank_size, 8);
	put_s(d, "\ntries="); put_x(d, BOOT_TRIES, 2);
	put_s(d, " flags="); put_x(d, BOOT_FLAGS, 2);
	{                                               /* the debug record (dbg.h): boot number, options, last crash */
		volatile struct dbg_area *g = (volatile struct dbg_area *)(NVRAM_BASE + DBG_OFFSET);

		put_s(d, " dbg="); put_x(d, g->seq, 8);
		put_s(d, " o="); put_x(d, g->options, 2);
		put_s(d, " c="); put_x(d, g->crash.magic == DBG_CRASH_MAGIC ? g->crash.vector : 0, 4);
#ifdef STAGE1
		put_s(d, " s=1");                       /* which boot loader answers: stage 1 (main flash) */
#else
		put_s(d, " s=0");                       /* the chip's */
#endif

	}
	put_s(d, "\n");
	for (id = 1; id < DUMP_R_COUNT; id++) {
		uintptr_t base;
		uint32_t size;

		region_bounds(id, &base, &size);
		put_s(d, "region="); put_u(d, id);
		put_s(d, " "); put_s(d, names[id]);
		put_s(d, " "); put_x(d, base, 8);
		put_s(d, " "); put_x(d, size, 8);
		put_s(d, "\n");
	}
}

/* ---- requests ---- */
static void do_speed(struct dump *d, uint8_t tag, const uint8_t *p)
{
	uint32_t want = get32(p), n, got, diff;
	uint8_t r[8] = { 0 };

	if (want < 1200 || want > 4000000) {
		send_error(d, DUMP_E_BAUD, DUMP_SPEED, tag);
		return;
	}
	n = (d->brgclk + 8 * want) / (16 * want);       /* nearest divider */
	if (n < 2 || n > 4096) {
		send_error(d, DUMP_E_BAUD, DUMP_SPEED, tag);
		return;
	}
	got = baud_of(d, n);
	diff = got > want ? got - want : want - got;
	if (diff * 100 > want * DUMP_BAUD_TOL_PCT) {
		send_error(d, DUMP_E_BAUD, DUMP_SPEED, tag);
		return;
	}
	put32(&r[4], got);
	if (send_frame(d, DUMP_SPEED | 0x80, tag, r, 8, 0, 0))
		return;
	tx_drain(d);                                    /* the reply goes out at the old speed */
	brg_set(d, n);
}

/* RUN / INSTALL: the first len bytes of the stage have this CRC and hold a bootable archive entry. */
static int staged_ok(struct dump *d, uint32_t len, uint32_t want, uint32_t *phys_out)
{
	uint32_t phys, virt, crc = 0xFFFFFFFF, off;
	uintptr_t a;
	struct arc_entry e;

	stage_where(&phys, &virt);
	if (phys == 0 || len == 0 || len > STAGE_SIZE)
		return 0;
	for (a = phys, off = len; off;) {               /* in pieces: watchdog, timer, RX */
		uint32_t k = off < 4096 ? off : 4096;

		crc = crc32_update(crc, a, k);
		a += k;
		off -= k;
		wdt_kick();
		(void)now_ms(d);
		rx_poll(d);
	}
	if (~crc != want || !arc_find_boot(phys, STAGE_SIZE, 0, 0, &e))
		return 0;
	*phys_out = phys;
	return 1;
}

static uint32_t stage_virt_of(uint32_t phys)
{
	uint32_t p, v;

	stage_where(&p, &v);
	return phys == p ? v : 0;
}

static void handle(struct dump *d)
{
	uint8_t type = d->rx[2], tag = d->rx[3];
	uint8_t p[24];                                  /* the fixed fields of a request, and of its reply */
	unsigned n = d->req_len, i;
	uintptr_t a;
	uint32_t off, len, crc;
	int rc;

	for (i = 0; i < n && i < DUMP_FIXED_MAX; i++)   /* requests are XORed with the tag */
		p[i] = d->rx[DUMP_HDR + i] ^ tag;
	if (tx_wait_idle(d))                            /* the last reply's buffers are free */
		return;

	switch (type) {
	case DUMP_HELLO:
		build_info(d);
		send_frame(d, DUMP_HELLO | 0x80, tag, 0, 0, DUMP_BOUNCE, d->text_len);
		return;
	case DUMP_PING:
		send_frame(d, DUMP_PING | 0x80, tag, 0, 0, 0, 0);
		return;
	case DUMP_BOOT:
		send_frame(d, DUMP_BOOT | 0x80, tag, 0, 0, 0, 0);
		d->boot = 1;
		return;
	case DUMP_SPEED:
		if (n != 4)
			break;
		do_speed(d, tag, p);
		return;
	case DUMP_WRITE:
		if (n < 9)
			break;
		if (p[0] != DUMP_R_STAGE) {
			send_error(d, DUMP_E_WRITE, type, tag);
			return;
		}
		off = get32(&p[4]);
		len = n - 8;
		rc = region(DUMP_R_STAGE, off, len, &a);
		if (rc) {
			send_error(d, (uint8_t)rc, type, tag);
			return;
		}
		put32(&p[8], len);                          /* the data is already in place (rx_byte) */
		put32(&p[12], ~crc32_update(0xFFFFFFFF, a, len));
		send_frame(d, DUMP_WRITE | 0x80, tag, p, 16, 0, 0);
		return;
	case DUMP_RUN:
	case DUMP_INSTALL: {
		uint32_t phys;

		if (n != 8)
			break;
		len = get32(&p[0]);
		if (!staged_ok(d, len, get32(&p[4]), &phys)) {
			send_error(d, DUMP_E_CHECK, type, tag);
			return;
		}
		if (type == DUMP_INSTALL) {
			uint32_t s = slot_where(0);

			if (!s || slot_install((const uint8_t *)phys, len)) {
				send_flash_error(d, type, tag);
				return;
			}
			if (~crc32_update(0xFFFFFFFF, s, len) != get32(&p[4])) {
				flash_fail.step = FLASH_STEP_VERIFY;
				flash_fail.addr = s;
				flash_fail.got = flash_fail.want = 0;
				send_flash_error(d, type, tag);
				return;
			}
			BOOT_TRIES = 0;                         /* the next boot tries the slot */
			BOOT_FLAGS = 0;
			send_frame(d, DUMP_INSTALL | 0x80, tag, 0, 0, 0, 0);
			return;
		}
		STAGE->phys = phys;
		STAGE->virt = stage_virt_of(phys);
		STAGE->len = len;
		STAGE->magic = STAGE_MAGIC;
		send_frame(d, DUMP_RUN | 0x80, tag, 0, 0, 0, 0);
		d->boot = 1;
		return;
	}
	case DUMP_OPTION:
		if (n != 2)
			break;
		switch (p[0]) {
		case OPT_SET_OPTIONS: dbg_set_options(p[1]); break;
		case OPT_SET_TRIES:   BOOT_TRIES = p[1];     break;
		case OPT_CLEAR_CRASH: dbg_clear_crash();     break;
		case OPT_SET_FLAGS:   BOOT_FLAGS = p[1];     break;
		case OPT_SET_S1_TRIES: dbg_set_s1_tries(p[1]); break;
		case OPT_TEST_FAULT:                         break;
		default:
			send_error(d, DUMP_E_RANGE, type, tag);
			return;
		}
		p[0] = dbg_options();
		p[1] = BOOT_TRIES;
		p[2] = BOOT_FLAGS;
		p[3] = 0;
		send_frame(d, DUMP_OPTION | 0x80, tag, p, 4, 0, 0);
		if (d->rx[DUMP_HDR] == (uint8_t)(OPT_TEST_FAULT ^ tag)) {
			(void)tx_wait_idle(d);                  /* the reply is out; now fault on purpose */
			__asm__ volatile("trap");
		}
		return;
	case DUMP_INSTALL_S1: {
		uint32_t phys, virt;

		if (n != 8)
			break;
		len = get32(&p[0]);
		stage_where(&phys, &virt);
		wdt_kick();
		if (!phys || len < S1_HDR || len > LOADER_SIZE ||
		    ~crc32_update(0xFFFFFFFF, phys, len) != get32(&p[4]) || !s1_valid(phys)) {
			send_error(d, DUMP_E_CHECK, type, tag);
			return;
		}
		if (loader_install((const uint8_t *)phys, len)) {
			send_flash_error(d, type, tag);
			return;
		}
		if (!s1_valid(loader_where())) {
			flash_fail.step = FLASH_STEP_VERIFY;
			flash_fail.addr = loader_where();
			flash_fail.got = flash_fail.want = 0;
			send_flash_error(d, type, tag);
			return;
		}
		dbg_set_s1_tries(0);
		send_frame(d, DUMP_INSTALL_S1 | 0x80, tag, 0, 0, 0, 0);
		return;
	}
	case DUMP_INSTALL_BOOT: {
		uint32_t phys, virt;
		struct arc_entry e;

		if (n != 8)
			break;
		if (running_from_bootflash()) {         /* stage 0 cannot erase the chip it runs from */
			flash_fail.step = FLASH_STEP_NONE;
			send_flash_error(d, type, tag);
			return;
		}
		len = get32(&p[0]);
		stage_where(&phys, &virt);
		wdt_kick();
		if (!phys || len == 0 || len > BOOT_SIZE || ~crc32_update(0xFFFFFFFF, phys, len) != get32(&p[4]) ||
		    !arc_find_boot(phys, len, 0, 0, &e)) {
			send_error(d, DUMP_E_CHECK, type, tag);
			return;
		}
		if (boot_flash_install((const uint8_t *)phys, len)) {
			send_flash_error(d, type, tag);
			return;
		}
		if (~crc32_update(0xFFFFFFFF, BOOT_BASE, len) != get32(&p[4])) {
			flash_fail.step = FLASH_STEP_VERIFY;
			flash_fail.addr = BOOT_BASE;
			flash_fail.got = flash_fail.want = 0;
			send_flash_error(d, type, tag);
			return;
		}
		send_frame(d, DUMP_INSTALL_BOOT | 0x80, tag, 0, 0, 0, 0);
		return;
	}
	case DUMP_UNINSTALL_S1:
		if (n != 0)
			break;
		if (loader_uninstall()) {
			send_flash_error(d, type, tag);
			return;
		}
		send_frame(d, DUMP_UNINSTALL_S1 | 0x80, tag, 0, 0, 0, 0);
		return;
	case DUMP_UNINSTALL:
		if (n != 0)
			break;
		if (slot_uninstall()) {
			send_flash_error(d, type, tag);
			return;
		}
		send_frame(d, DUMP_UNINSTALL | 0x80, tag, 0, 0, 0, 0);
		return;
	case DUMP_READ:
	case DUMP_CRC:
		if (n != 12)
			break;
		off = get32(&p[4]);
		len = get32(&p[8]);
		if (type == DUMP_READ && (len == 0 || len > DUMP_BLOCK_MAX)) {
			send_error(d, DUMP_E_RANGE, type, tag);
			return;
		}
		rc = region(p[0], off, len, &a);
		if (rc) {
			send_error(d, (uint8_t)rc, type, tag);
			return;
		}
		if (type == DUMP_READ) {
			send_frame(d, DUMP_READ | 0x80, tag, p, 12, a, len);
			return;
		}
		crc = 0xFFFFFFFF;
		while (len) {                               /* in pieces: watchdog, timer, RX */
			uint32_t k = len < 4096 ? len : 4096;

			crc = crc32_update(crc, a, k);
			a += k;
			len -= k;
			wdt_kick();
			(void)now_ms(d);
			rx_poll(d);
		}
		put32(&p[12], ~crc);
		send_frame(d, DUMP_CRC | 0x80, tag, p, 16, 0, 0);
		return;
	default:
		send_error(d, DUMP_E_TYPE, type, tag);
		return;
	}
	send_error(d, DUMP_E_LEN, type, tag);
}

/* ---- the window ---- */
static int listen(struct dump *d, const char *magic, uint32_t ms)
{
	uint32_t t0 = now_ms(d);
	unsigned m = 0;
	int c;

	while (now_ms(d) - t0 < ms) {
		wdt_kick();
		rx_poll(d);
		while ((c = rx_pop(d)) >= 0) {
			m = c == magic[m] ? m + 1 : c == magic[0];
			if (m == DUMP_MAGIC_LEN)
				return 1;
		}
	}
	return 0;
}

static void serve(struct dump *d)
{
	uint32_t last = now_ms(d), t_byte = last, now;
	int c;

	send_line(d, d->scc ? "MQDUMP READY 1 SCC1 115200\r\n" : "MQDUMP READY 1 SMC1 115200\r\n");
	while (!d->boot && !d->dead) {
		wdt_kick();
		rx_poll(d);
		now = now_ms(d);
		if (d->rx_len && now - t_byte > DUMP_GAP_MS)
			d->rx_len = 0;                          /* a lost character: drop the partial request */
		while (!d->boot && !d->dead && (c = rx_pop(d)) >= 0) {
			t_byte = now;
			if (rx_byte(d, (uint8_t)c)) {
				handle(d);
				last = now_ms(d);
			}
		}
		now = now_ms(d);
		if (d->n != d->n0 && now - last > DUMP_FALLBACK_MS) {
			brg_set(d, d->n0);                      /* nobody at the new speed: go back */
			last = now;
		}
		if (now - last > DUMP_IDLE_MS)
			break;
	}
	tx_drain(d);
	if (d->n != d->n0)
		brg_set(d, d->n0);
	send_line(d, "\r\nMQDUMP CLOSED\r\n");
	tx_drain(d);
}

static int window(const char *magic, uint32_t ms)
{
	struct dump d;
	volatile uint8_t *z = (volatile uint8_t *)&d;
	unsigned i;
	int served;

	for (i = 0; i < sizeof d; i++)
		z[i] = 0;
	d.scc = rs232_on_scc1();
	/* clocks as the app works them out (pll_out_hz_maybe, brg_clock_hz_maybe) */
	d.gclk = cfg32(CFG_BUF, CFG_OSCCLK) * (((PLPRCR >> 20) & 0xFF) + 1);
	d.brgclk = d.gclk >> ((SCCR >> 10) & 6);
	d.n0 = d.brgclk / 16 / DUMP_BAUD;               /* truncated, as cpm_brg_value */
	if (d.n0 < 2 || d.n0 > 4096)                    /* clock makes no sense: no window */
		return 0;

	icache(1);
	clock_start(&d);
	uart_start(&d);
	served = listen(&d, magic, ms);
	if (served)
		serve(&d);
	uart_stop(&d);
	clock_stop(&d);
	icache(0);
	return served;
}

void dump_window(void)
{
	(void)window(DUMP_MAGIC, DUMP_LISTEN_MS);
}

/* Stage 0 with a stage 1 installed (serdump.h): a short listen for the recovery magic only. */
int dump_window_recovery(void)
{
	return window(DUMP_RECOVERY_MAGIC, DUMP_RECOVERY_MS);
}

/* A stage-1 image at a (the loader sector, or the stage): magic, a length that fits, and the code's CRC. */
int s1_valid(uintptr_t a)
{
	const volatile struct s1_header *h = (const volatile struct s1_header *)a;

	if (h->magic != S1_MAGIC || h->len == 0 || h->len > LOADER_SIZE - S1_HDR || h->entry < S1_HDR ||
	    h->entry >= S1_HDR + h->len)
		return 0;
	return ~crc32_update(0xFFFFFFFF, a + S1_HDR, h->len) == h->crc;
}
