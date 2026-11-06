/* Board hardware layer: watchdog, clock tree, cache maintenance, panel FPGA configuration, panel pins and timers, and
 * the IDMA channel that feeds the panel. Polled, except one interrupt: the IDMA completion that keeps the
 * panel continuously fed (idma_isr), as the stock ledpanel driver does. */
#include "hw.h"
#include "fpga_bits.h"

/* ---- watchdog and clocks ---- */
void wdt_kick(void)
{
	HW16(0x0E) = 0x556C;
	HW16(0x0E) = 0xAA39;
}

static uint32_t pll_out_hz(void)
{
	/* the boot loader's hand-off block holds the PLL input clock at RAM 0x1C */
	return *(volatile uint32_t *)0x1Cu * (((HW32(0x284) >> 20) & 0xFFu) + 1u);
}
uint32_t brg_clock_hz(void) { return pll_out_hz() >> ((HW32(0x280) >> 10) & 6u); }
uint32_t cpu_clock_hz(void)
{
	uint32_t s = HW32(0x280);
	uint32_t div = (HW32(0x284) & 0x400u) ? ((s >> 8) & 7u) + 1u : (s >> 5) & 7u;
	return pll_out_hz() >> div;
}

void delay_us(uint32_t us)
{
	/* a floor, not a precise delay: each loop takes at least one cycle */
	uint32_t n = (cpu_clock_hz() / 1000000u) * us;
	for (volatile uint32_t i = 0; i < n; i++)
		;
}

void cache_flush(const void *p, uint32_t n)
{
	uint32_t a = (uint32_t)p & ~15u, e = (uint32_t)p + n;
	for (; a < e; a += 16)
		__asm__ volatile ("dcbf 0,%0" : : "r"(a));
	__asm__ volatile ("sync");
}

void cpm_command(uint16_t op)
{
	while (HW16(0x9C0) & 1u)   /* wait for any previous CPM command to finish, like the stock cpm_command (0x5060) */
		wdt_kick();
	HW16(0x9C0) = op | 1u;     /* issue (FLG set); do NOT wait for completion — the stock does not, and waiting
	                            * for the flag to re-clear here hangs panel_init's IDMA init on the real CPM */
}

/* ---- panel FPGA: Xilinx slave-serial configuration over the CPM SPI ---- */
#define PADIR  HW16(0x950)
#define PAPAR  HW16(0x952)
#define PADAT  HW16(0x956)
#define PBDIRL HW16(0xABA)
#define PBPARL HW16(0xABE)
#define PBODRL HW16(0xAC2)
#define PBDATL HW16(0xAC6)
#define PIPR   HW32(0x0F0)
#define PA_PROG 0x0800u     /* PA4 */
#define PB_INIT 0x4000u     /* PB17 */
#define PB_LOAD 0x1000u     /* PB19, low during the download */
#define PIPR_DONE 0x02000000u

struct sbd { volatile uint16_t status, length; volatile uint32_t buf; };
#define SPI_RXBD_OFF 0x2810u
#define SPI_TXBD_OFF 0x2818u
#define SPI_RXBD ((struct sbd *)(IMMR + SPI_RXBD_OFF))
#define SPI_TXBD ((struct sbd *)(IMMR + SPI_TXBD_OFF))

static uint8_t spi_rx[FPGA_BITS_LEN + 16] __attribute__((aligned(16)));

/* SPMODE for 1 MHz or the nearest below it: PM from BRGCLK, directly or through /16 */
static uint16_t spi_mode(uint16_t mode, uint32_t hz)
{
	uint32_t brg = brg_clock_hz(), act[2];
	uint16_t pm[2];
	for (int d = 0; d < 2; d++) {
		uint32_t clk = d ? (brg + 8) >> 4 : brg;
		uint32_t v = ((clk + (hz >> 1)) / hz + 2) >> 2;
		v = v ? v - 1 : 0;
		if (v > 15) v = 15;
		pm[d] = (uint16_t)v;
		act[d] = (d ? brg >> 4 : brg) / (v * 4 + 4);
	}
	uint32_t e0 = act[0] > hz ? act[0] - hz : hz - act[0];
	uint32_t e1 = act[1] > hz ? act[1] - hz : hz - act[1];
	if ((act[0] <= hz || act[1] <= hz) && (hz <= act[0] || hz <= act[1])) {
		if (hz < act[0]) e0 = 0xFFFFFFFFu;
		else             e1 = 0xFFFFFFFFu;
	}
	return (uint16_t)((e1 < e0 ? (uint16_t)(mode | pm[1] | 0x800u) : (uint16_t)(mode | pm[0])) | 0x100u);
}

const char *fpga_load(void)
{
	PAPAR &= (uint16_t)~PA_PROG;
	PADIR |= PA_PROG;
	PBPARL &= (uint16_t)~(PB_INIT | PB_LOAD);
	PBDIRL = (uint16_t)((PBDIRL & ~PB_INIT) | PB_LOAD);
	PBDATL |= PB_LOAD;
	PADAT |= PA_PROG;
	delay_us(100);

	/* SPI pins, parameter RAM and descriptors */
	PBPARL |= 0x000E;
	PBDIRL |= 0x000E;
	PBODRL &= 0xFFF1;
	HW16(0xAA0) = 0;
	HW8(0x3D84) = 0x10;
	HW8(0x3D85) = 0x10;
	HW16(0x3D80) = SPI_RXBD_OFF;
	HW16(0x3D82) = SPI_TXBD_OFF;
	HW8(0xAAA) = 0;
	HW8(0xAA6) = 0xFF;
	wdt_kick();
	uint16_t spmode = spi_mode(0x670, 1000000u);   /* computed here, master enabled just before the download */

	for (int attempt = 0; attempt < 8; attempt++) {
		PADAT &= (uint16_t)~PA_PROG;             /* PROG low clears the FPGA */
		delay_us(300);
		PADAT |= PA_PROG;
		if (PIPR & PIPR_DONE)
			continue;                            /* DONE must drop while unconfigured */
		/* Wait for INIT (PB17) to rise after the clear, but do NOT hang on it: on this board the line does not
		 * read back high even when the FPGA is ready and configures correctly, so fall through after a short
		 * settle and let the DONE + readback check below confirm success. (The simulator always forced INIT
		 * high, which is why this only surfaced on real hardware.) */
		for (uint32_t n = 0; !(PBDATL & PB_INIT) && n < 200000u; n++)
			wdt_kick();
		delay_us(1000);
		PBDATL &= (uint16_t)~PB_LOAD;
		delay_us(100);
		HW16(0xAA0) = spmode;                    /* enable the SPI master for the download */

		cache_flush(spi_rx, sizeof spi_rx);
		cache_flush(fpga_bits, FPGA_BITS_LEN);
		HW16(0x3D86) = FPGA_BITS_LEN;
		SPI_RXBD->length = 0;
		SPI_RXBD->buf = (uint32_t)spi_rx;
		SPI_RXBD->status = 0xB000;
		SPI_TXBD->length = FPGA_BITS_LEN;
		SPI_TXBD->buf = (uint32_t)fpga_bits;
		SPI_TXBD->status = 0xB800;
		cpm_command(0x0051);                     /* INIT RX AND TX, SPI */
		HW8(0xAAA) = 0;
		HW8(0xAA6) = 0xFF;
		HW8(0xAAD) = 0x80;                       /* start */
		for (uint32_t n = 0; SPI_RXBD->status & 0x8000u; n++) {
			wdt_kick();
			if (n > 20000000u)
				return "FPGA SPI transfer never finished";
		}
		uint32_t got = SPI_RXBD->length;
		cache_flush(spi_rx, sizeof spi_rx);
		int bad = !got;
		for (uint32_t i = 0; i < got && !bad; i++)
			if (spi_rx[i] != 0xFF)
				bad = 1;
		int done = (PIPR & PIPR_DONE) != 0;
		PBDATL |= PB_LOAD;
		if (done && !bad)
			return 0;
	}
	return "FPGA did not configure after 8 attempts";
}

/* ---- panel output ---- */
#define PCDIR  HW16(0x960)
#define PCPAR  HW16(0x962)
#define PCSO   HW16(0x964)
#define PCINT  HW16(0x968)
#define RCCR   HW16(0x9C4)
#define TGCR   HW16(0x980)
#define IDSR1  HW8(0x910)
#define IDMR1  HW8(0x914)
#define PB_OUTEN 0x0080u    /* PB24: held high until the first frame */
#define PANEL_BASE 0xF0000000u

struct ibd { volatile uint16_t status; volatile uint8_t dfcr, sfcr; volatile uint32_t length, src, dst; };
#define IBD_OFF 0x2800u
#define IBD ((struct ibd *)(IMMR + IBD_OFF))
#define IBD_V 0x8000u

#define NFB 3                                   /* a small frame ring, like the stock 8-deep ledpanel_framebuf */
static uint16_t fbring[NFB][FRAME_W * FRAME_H] __attribute__((aligned(16)));
static volatile uint8_t fb_rdy[NFB];            /* 1 = holds a frame to show, 0 = free */
static volatile int fb_wi, fb_ri;              /* the app fills fb_wi; the IDMA ISR shows fb_ri */
static volatile int g_idma_on;
static uint32_t g_sent;
static int g_running;

static void idma_arm(uint16_t *fb)
{
	if (IBD->status & IBD_V)                        /* a transfer is still running: leave it */
		return;
	IBD->src = (uint32_t)fb;
	IBD->dst = PANEL_BASE;
	IBD->length = (uint32_t)sizeof fbring[0];
	IBD->status |= IBD_V;                           /* V: the CP streams this frame to the FPGA */
}

uint32_t panel_frames_sent(void) { return g_sent; }

/* timer n (3 or 4) registers: TMR, TRR, TCN, TER */
#define TMR(t) HW16((t) == 3 ? 0x9A0 : 0x9A2)
#define TRR(t) HW16((t) == 3 ? 0x9A4 : 0x9A6)
#define TCN(t) HW16((t) == 3 ? 0x9AC : 0x9AE)
#define TER(t) HW16((t) == 3 ? 0x9B4 : 0x9B6)

void panel_init(void)
{
	PBDATL |= PB_OUTEN;
	PBDIRL |= PB_OUTEN;
	PBPARL &= (uint16_t)~PB_OUTEN;
	PAPAR |= 0xE000;                                 /* PA0-2; PA3 stays a plain input, as the board ends up */
	PADIR = (uint16_t)((PADIR & 0xAFFF) | 0xA000);

	/* the write the board needs with chip select 1 briefly invalid; the target is in the hand-off block at RAM 0x24 */
	/* The decompile had a write of 2 to *RAM[0x24] (the NVRAM at 0xF8780000) with chip-select 1 briefly invalid.
	 * On this board that no-responder store intermittently trips the bus monitor (TEA -> checkstop) or hangs,
	 * and the panel comes up correctly without it, so it is omitted. */

	/* IDMA1: one wrap descriptor, memory to the panel, DREQ0 on PC15, acknowledge on PC5 */
	cpm_command(0x0B11);
	PCSO |= 0x0001;
	PCPAR = (uint16_t)((PCPAR & ~0x0001u) | 0x0400u);
	PCDIR = (uint16_t)((PCDIR & ~0x0001u) | 0x0400u);
	RCCR = (uint16_t)((RCCR & ~0x0070u) | 0x0040u);
	PCINT &= (uint16_t)~0x0001u;
	RCCR |= 0x0020;
	IBD->status = 0x3800;
	IBD->dfcr = 0x14;
	IBD->sfcr = 0x14;
	IBD->length = 0;
	IBD->src = 0;
	IBD->dst = 0;
	HW16(0x3CC2) = 0x0005;
	HW16(0x3CC0) = IBD_OFF;
	cpm_command(0x0511);
	IDSR1 = 7;
	IDMR1 = 7;

	/* panel timers: 3 a 280 ns square wave, 4 restarting after 0x74 * 0x300 of its edges */
	uint32_t half = (uint32_t)((uint64_t)280u * cpu_clock_hz() / 1000000000u) >> 1;
	TGCR &= 0x70FF;
	TCN(3) = 0;
	TER(3) = 0xFFFF;
	TMR(3) = 0x002A;
	TRR(3) = (uint16_t)(half - 1u);
	uint32_t c = 0x74u * 0x300u - 1u, pre = c >> 16;
	TGCR &= 0x07FF;
	TCN(4) = 0;
	TER(4) = 0xFFFF;
	TMR(4) = (uint16_t)((pre << 8) | 0x0Eu);
	TRR(4) = (uint16_t)(c / (pre + 1u));
	TGCR |= 0x1100;

	/* Route the IDMA1 completion interrupt to the external-interrupt vector (0x500) and take it. The ISR
	 * (idma_isr, via start.S _extint) re-arms the DMA every completion so the FPGA is never starved between
	 * frames -- the stock driver does the same with its IDMA interrupt. IDMR1 (set above) enables the source. */
	HW32(0x948) |= 0x00200000u;                     /* CIMR: unmask the IDMA1 CPM source (bit 21) */
	HW32(0x940) |= 0x00005F80u;                     /* CICR: IEN + IRL = 2 (CPM requests at SIU level 2) */
	HW32(0x014) |= 0x04000000u;                     /* SIMASK: unmask SIU level 2 (SIPEND bit 5) */
	uint32_t msr;
	__asm__ volatile("mfmsr %0" : "=r"(msr));
	__asm__ volatile("mtmsr %0; isync" : : "r"(msr | 0x8000u));   /* MSR[EE]: take external interrupts */
}

int panel_ready(void) { return fb_rdy[fb_wi] == 0; }   /* a free write slot exists */

void panel_blank(void)
{
	PBDATL |= PB_OUTEN;
	g_running = 0;
	g_idma_on = 0;
}

void panel_present(const uint16_t *px, int w, int h)
{
	int k = fb_wi;
	for (uint32_t n = 0; fb_rdy[k]; n++) {             /* ring full: wait for the ISR to free a buffer */
		wdt_kick();
		if (n > 400000u) {
			if (!(PCSO & 1u))
				PCSO |= 1u;                          /* the panel stalled: re-assert DREQ */
			n = 0;
		}
	}
	uint16_t *fb = fbring[k];
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			fb[y * FRAME_W + x] = px[y * w + x];
	fb[(sizeof fbring[0] - 8) / 2] |= 0x8000u;        /* the end-of-frame marker */
	cache_flush(fb, (uint32_t)sizeof fbring[0]);

	uint32_t msr;                                     /* update the ring with the IDMA interrupt held off */
	__asm__ volatile("mfmsr %0" : "=r"(msr));
	__asm__ volatile("mtmsr %0; isync" : : "r"(msr & ~0x8000u));
	fb_rdy[k] = 1;
	if (!g_idma_on) {                                 /* the first frame starts the stream; the ISR keeps it going */
		fb_ri = k;
		idma_arm(fb);
		if (!g_running) {
			PBDATL &= (uint16_t)~PB_OUTEN;
			g_running = 1;
		}
		g_idma_on = 1;
	}
	fb_wi = (k + 1) % NFB;
	__asm__ volatile("mtmsr %0; isync" : : "r"(msr));
	g_sent++;
}

/* IDMA1 completion: free the shown frame when a newer one is queued, then re-arm the DMA with the current
 * frame (re-sending it when nothing newer is ready) so the FPGA is never starved. From start.S _extint. */
void idma_isr(void)
{
	IDSR1 = 7;                                        /* clear the IDMA done status */
	if (!(PCSO & 1u))
		PCSO |= 1u;                                   /* keep DREQ asserted, as the stock ISR does */
	int next = (fb_ri + 1) % NFB;
	if (fb_rdy[next]) {                               /* a newer frame is queued: advance to it */
		fb_rdy[fb_ri] = 0;
		fb_ri = next;
	}
	idma_arm(fbring[fb_ri]);
	HW32(0x94C) = 0x00200000u;                        /* CISR: end this source's service (EOI) */
}

/* the external-interrupt vector (start.S, 0x500) lands here */
void ext_interrupt(void)
{
	HW16(0x930) = 1;                                  /* CIVR IACK, then read: moves the source from CIPR to CISR */
	(void)HW16(0x930);
	idma_isr();
}

/* ---- reset ---- */
void hw_reset(void)
{
	uint32_t m;
	panel_blank();
	__asm__ volatile("mfmsr %0" : "=r"(m));
	__asm__ volatile("mtmsr %0; isync" : : "r"(m & ~0x8000u));
	HW32(0x284) |= 0x80u;                            /* PLPRCR: reset the CPU */
	for (;;)
		;                                            /* and no more kicks: the boot loader armed the watchdog */
}

/* ---- the main flash: four 8 MB pages, data side, cache-inhibited and guarded. Our code is identity-mapped in the first
 * SDRAM bank, so it keeps running with translation off while the entries go in. Nothing else loads TLB entries, as we
 * take no misses; a miss is a fault report (start.S). ---- */
#define MTSPR(n, v) __asm__ volatile("mtspr %0, %1" : : "n"(n), "r"(v))
void mmu_map_main_flash(void)
{
	uint32_t m, base = *(volatile uint32_t *)0x3Cu;      /* the boot loader's hand-off: flash 1 base */
	__asm__ volatile("mfmsr %0" : "=r"(m));
	__asm__ volatile("mtmsr %0; isync" : : "r"(m & ~0x30u));
	/* MD_CTR: the boot loader's setting, with the next entry just below what it loaded: the IMMR at index 31, the boot
	 * flash at 30, then one 8 MB entry per SDRAM page from 29 down (hand-off word 0x14: the RAM it mapped). */
	uint32_t pages = (*(volatile uint32_t *)0x14u + 0x7FFFFFu) >> 23;
	MTSPR(792, 0x3C000000u | (29u - pages) << 8);
	for (uint32_t p = 0; p < 4; p++) {
		uint32_t a = base + p * 0x800000u;
		MTSPR(795, a | 0x200u);                          /* MD_EPN */
		MTSPR(797, 0x0Fu);                               /* MD_TWC: valid, 8 MB, guarded */
		MTSPR(798, a | 0x9FFu);                          /* MD_RPN: cache-inhibited */
	}
	__asm__ volatile("mtmsr %0; isync" : : "r"(m));
}

/* ---- the time base ---- */
#define TBSCR HW16(0x200)
void time_init(void)
{
	HW32(0x280) |= 0x02000000u;                      /* SCCR.TBS: the time base runs from GCLK2 / 16 */
	TBSCR |= 1u;
}
uint64_t time_ticks(void)
{
	/* The real mftb opcodes, as words: the assembler spells mftb as mfspr 268/269, which the MPC8xx traps (software
	 * emulation, vector 0x1000), so reading the clock on the sign crashed it. Upper, lower, upper again for a carry. */
	uint32_t hi, lo, again;
	do {
		__asm__ volatile(".long 0x7C6D42E6\n\t.long 0x7C8C42E6\n\t.long 0x7CAD42E6\n\tmr %0, 3\n\tmr %1, 4\n\tmr %2, 5"
		                 : "=r"(hi), "=r"(lo), "=r"(again) : : "r3", "r4", "r5");
	} while (hi != again);
	return (uint64_t)hi << 32 | lo;
}
uint32_t time_hz(void)
{
	/* SCCR.TBS: GCLK2 / 16, else the oscillator / 4 */
	return (HW32(0x280) & 0x02000000u) ? cpu_clock_hz() / 16u : *(volatile uint32_t *)0x1Cu / 4u;
}
