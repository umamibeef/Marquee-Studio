/* Network window for BOOTSTRAP V2.00 (C rebuild), built with `make NET=1`.
 *
 * After SDRAM is up and before the app is loaded, the bootloader:
 *  1. brings up the FEC and the PHY; with no PHY or no link within NET_LINK_MS it carries on
 *     booting normally;
 *  2. gets an address by DHCP, or falls back to NET_STATIC_IP after NET_DHCP_MS;
 *  3. answers ARP and ping, and serves TFTP for NET_WINDOW_MS (longer while transfers run):
 *       get bootflash.bin   the 512 KB boot flash
 *       get flash1.bin      main flash bank 1     get flash2.bin  main flash bank 2
 *       get nvram.bin       the battery-backed RAM (settings, boot counter)
 *       get eeprom_a0.bin   board config EEPROM (variant, memory set-up, panel size)
 *       get eeprom_a2.bin   UPM program (fixed boards) or SDRAM SPD (SPD boards)
 *       get info.txt        board, clock, memory, PHY and network details
 *       get inputs.txt      live levels of the app's 4 switch inputs and the strap pins
 *       put <any name>      an archive with a bootable entry (e.g. a whole bootflash image,
 *                           or one entry from tools/mkapp.py); it is booted from RAM at once.
 * Nothing is ever written to flash.
 *
 * Runs on its own stack in SDRAM; lwIP's globals live in SDRAM too (link_net.ld). */
#include "board.h"
#include "netboot.h"
#include "fec.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/apps/tftp_server.h"
#include "netif/ethernet.h"
#include "libc/string.h"

/* ---- clock: CPM timer 2 free-running at sysclk / 250 ---- */
static uint32_t ticks_per_ms, last_tcn, acc_ticks, rand_state;

static void clock_init(uint32_t sysclk)
{
	ticks_per_ms = sysclk / 250 / 1000;
	if (!ticks_per_ms)
		ticks_per_ms = 1;
	TGCR &= (uint16_t)~0x00F0;          /* timer 2: stopped, not cascaded */
	TMR2 = 0xF902;                      /* prescale 250, system clock, free run */
	TRR2 = 0xFFFF;
	TCN2 = 0;
	TER2 = 0xFFFF;
	TGCR |= 0x0010;                     /* RST2: run */
	last_tcn = 0;
	acc_ticks = 0;
}

uint32_t net_now(void)
{
	uint16_t now = TCN2;

	acc_ticks += (uint16_t)(now - last_tcn);
	last_tcn = now;
	return acc_ticks / ticks_per_ms;
}

uint32_t sys_now(void)
{
	return net_now();
}

unsigned int netboot_rand(void)
{
	rand_state = rand_state * 1103515245u + 12345u + TCN2;
	return rand_state >> 8;
}

/* ---- text helpers for info.txt ---- */
static char info[640];
static unsigned info_len;
static const char *last_upload = "none";

static void put_s(const char *s)
{
	while (*s && info_len < sizeof info - 1)
		info[info_len++] = *s++;
}

static void put_u(uint32_t v)
{
	char b[11];
	int i = 10;

	b[i] = 0;
	do
		b[--i] = (char)('0' + v % 10);
	while ((v /= 10) && i);
	put_s(&b[i]);
}

static void put_x(uint32_t v, int digits)
{
	char b[9];
	int i;

	for (i = 0; i < digits; i++)
		b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 0xF];
	b[digits] = 0;
	put_s(b);
}

/* Board families, from config EEPROM bytes 4/5 (the same tests the app makes):
 *   SPD board  (byte4 == 1, byte5 <  0x50): memory from SPD, panel size in byte 0x14 (app FUN_00055aa0)
 *   Port C/B panel (byte4 == 2, or byte4 == 1 and byte5 >= 0x50): the app drives its LED-panel
 *                control lines on Port C/B, leaving Port D free for the FEC (app FUN_00055acc)
 *   otherwise: the app drives them on PD9 and PD3, which are FEC MII pins. */
static int spd_board(void)
{
	return cfg8(CFG_BUF, CFG_MEM_TYPE) == 1 && cfg8(CFG_BUF, CFG_MEM_SUB) < 0x50;
}

static int portc_panel(void)
{
	uint8_t t = cfg8(CFG_BUF, CFG_MEM_TYPE);

	return t == 2 || (t == 1 && cfg8(CFG_BUF, CFG_MEM_SUB) >= 0x50);
}

/* Panel geometry the app will use (FUN_00055b80 / FUN_00055be8) */
static void panel_size(uint32_t *w, uint32_t *h)
{
	if (!spd_board()) {
		uint32_t o = cfg32(CFG_BUF, CFG_OPTIONS);

		*w = ((o >> 4) & 0xF) * 20 + 20;
		*h = (o & 0xF) * 16 + 16;
	} else {
		uint8_t g = cfg8(CFG_BUF, 0x14);

		*w = (g & 0x80) ? ((g >> 4) & 7) * 40 + 40 : 200;
		*h = (g & 0x80) ? (g & 0xF) * 16 + 16 : 128;
	}
}

static struct netif nif;
static uint8_t mac[6];
static int static_ip;

static void build_info(void)
{
	const ip4_addr_t *ip = netif_ip4_addr(&nif);
	int i;

	info_len = 0;
	put_s("MARQUEE BOOTSTRAP V2.00 (C rebuild) with network window\r\n");
	put_s("board id:   0x"); put_x(cfg32(CFG_BUF, CFG_BOARD_ID), 8);
	put_s("\r\nsysclk:     "); put_u(BI->sysclk);
	put_s(" Hz\r\nsdram:      "); put_u(BI->sdram_total >> 20);
	put_s(" MB\r\nflash1:     0x"); put_x(BI->flash1_base, 8); put_s(" + "); put_u(BI->flash1_total >> 10);
	put_s(" KB\r\nflash2:     0x"); put_x(BI->flash2_base, 8); put_s(" + "); put_u(BI->flash2_total >> 10);
	put_s(" KB\r\nnvram:      0x"); put_x(BI->nvram_base, 8); put_s(" + "); put_u(BI->nvram_size >> 10);
	put_s(" KB\r\nboot tries: 0x"); put_x(BOOT_TRIES, 2); put_s(" flags 0x"); put_x(BOOT_FLAGS, 2);
	put_s("\r\nmac:        ");
	for (i = 0; i < 6; i++) {
		put_x(mac[i], 2);
		if (i < 5)
			put_s(":");
	}
	put_s("\r\nip:         ");
	for (i = 0; i < 4; i++) {
		put_u(ip4_addr_get_byte(ip, i));
		if (i < 3)
			put_s(".");
	}
	put_s(static_ip ? " (static fallback)" : " (dhcp)");
	put_s("\r\nphy:        addr "); put_u((uint32_t)fec_stat.phy_addr);
	put_s(" id 0x"); put_x(fec_stat.phy_id, 8);
	put_s(fec_stat.speed100 ? " 100" : " 10"); put_s(fec_stat.full_duplex ? "M full\r\n" : "M half\r\n");
	{
		uint32_t pw, ph;

		panel_size(&pw, &ph);
		put_s("config:     byte4 0x"); put_x(cfg8(CFG_BUF, CFG_MEM_TYPE), 2);
		put_s(" byte5 0x"); put_x(cfg8(CFG_BUF, CFG_MEM_SUB), 2);
		put_s(" options 0x"); put_x(cfg32(CFG_BUF, CFG_OPTIONS), 8);
		put_s(spd_board() ? " (SPD memory board)" : " (fixed memory board)");
		put_s("\r\npanel:      "); put_u(pw); put_s(" x "); put_u(ph);
		put_s(" (size the app derives from the config EEPROM)\r\nportD:      ");
		put_s(portc_panel() ? "app keeps its panel control lines off Port D: FEC pins free for Ethernet\r\n"
		                    : "app uses PD3/PD9 (FEC MII pins) for the panel: Ethernet only during boot\r\n");
	}
	put_s("last upload: "); put_s(last_upload);
	put_s("\r\nget: bootflash.bin flash1.bin flash2.bin nvram.bin eeprom_a0.bin eeprom_a2.bin info.txt inputs.txt\r\n"
	      "put: any archive with a bootable entry -> booted from RAM\r\n");
}

/* Live levels of the inputs the app's I/O code defines (FUN_00058e74.., all active low) and
 * the raw port and PCMCIA-pin registers, so jumpers and contacts can be found by toggling them
 * between two reads. Port D carries the FEC during the window, so PD8 is not readable here. */
static void put_bit(const char *name, uint32_t reg, uint32_t mask)
{
	put_s(name);
	put_s((reg & mask) ? "1 (open)\r\n" : "0 (closed to ground)\r\n");
}

static void build_inputs(void)
{
	uint16_t pa = PADAT, pc = PCDAT;
	uint32_t pb = PBDAT, pipr = PIPR;

	info_len = 0;
	put_s("raw: PADAT 0x"); put_x(pa, 4); put_s(" PBDAT 0x"); put_x(pb, 8);
	put_s(" PCDAT 0x"); put_x(pc, 4); put_s(" PDDAT 0x"); put_x(PDDAT, 4);
	put_s(" PIPR 0x"); put_x(pipr, 8);
	put_s("\r\napp inputs (active low; the production-test jumper selects 18-step vs 3-step diagnostics):\r\n");
	put_bit("  in0 PB16 (Port C/B boards: production-test jumper): ", pb, 0x00008000);
	put_bit("  in1 PC14:                                  ", pc, 0x0002);
	put_bit("  in2 PA10 (other boards: production-test jumper):  ", pa, 0x0020);
	put_bit("  in3 PA11:                                  ", pa, 0x0010);
	put_s("boot straps: PIPR[31:24] 0x"); put_x(pipr >> 24, 2);
	put_s(" PBDAT[17:14] 0x"); put_x((pb >> 14) & 0xF, 1);
	put_s(" PDDAT[11:9] (FEC owns Port D now)\r\n");
}

/* ---- TFTP ---- */
struct xfer {
	int      upload;
	uintptr_t base;
	uint32_t size, pos;
};

static struct xfer xf;
static int busy, boot_ready;
static uint32_t last_activity;
static struct arc_entry found;

static void *t_open(const char *fname, const char *mode, u8_t write)
{
	(void)mode;
	if (busy)
		return NULL;
	memset(&xf, 0, sizeof xf);
	if (write) {
		xf.upload = 1;
		xf.base = NET_UPLOAD_BASE;
		xf.size = NET_UPLOAD_MAX;
	} else if (!strcmp(fname, "bootflash.bin")) {
		xf.base = BI->boot_base;
		xf.size = BI->boot_size;
	} else if (!strcmp(fname, "flash1.bin")) {
		xf.base = BI->flash1_base;
		xf.size = BI->flash1_total;
	} else if (!strcmp(fname, "flash2.bin")) {
		xf.base = BI->flash2_base;
		xf.size = BI->flash2_total;
	} else if (!strcmp(fname, "nvram.bin")) {
		xf.base = BI->nvram_base;
		xf.size = BI->nvram_size;
	} else if (!strcmp(fname, "eeprom_a0.bin")) {      /* board config EEPROM, as read at boot */
		xf.base = CFG_BUF;
		xf.size = 256;
	} else if (!strcmp(fname, "eeprom_a2.bin")) {      /* UPM program or SDRAM SPD EEPROM */
		xf.base = spd_board() ? SPD_BUF : UPM_BUF;
		xf.size = 256;
	} else if (!strcmp(fname, "inputs.txt")) {         /* live pin levels, for jumper hunting */
		build_inputs();
		xf.base = (uintptr_t)info;
		xf.size = info_len;
	} else if (!strcmp(fname, "info.txt")) {
		build_info();
		xf.base = (uintptr_t)info;
		xf.size = info_len;
	} else {
		return NULL;
	}
	busy = 1;
	last_activity = net_now();
	return &xf;
}

static int t_read(void *h, void *buf, int bytes)
{
	struct xfer *x = h;
	uint32_t n = x->size - x->pos;
	uint32_t i;

	if (n > (uint32_t)bytes)
		n = (uint32_t)bytes;
	for (i = 0; i < n; i++)                       /* byte reads: flash and NVRAM are 8/16-bit */
		((uint8_t *)buf)[i] = *(volatile const uint8_t *)(x->base + x->pos + i);
	x->pos += n;
	last_activity = net_now();
	return (int)n;
}

static int t_write(void *h, struct pbuf *p)
{
	struct xfer *x = h;

	if (x->pos + p->tot_len > x->size)
		return -1;
	pbuf_copy_partial(p, (void *)(x->base + x->pos), p->tot_len, 0);
	x->pos += p->tot_len;
	last_activity = net_now();
	return 0;
}

static void t_close(void *h)
{
	struct xfer *x = h;

	busy = 0;
	last_activity = net_now();
	if (!x->upload)
		return;
	/* terminate the chain so the walk stops after the upload */
	memset((void *)(x->base + x->pos), 0, 8);
	if (arc_find_boot(x->base, x->pos + 4, 0, 0, &found)) {
		boot_ready = 1;
		last_upload = "bootable entry found, booting it";
	} else {
		last_upload = "rejected: no bootable entry with a valid check word";
	}
}

static void t_error(void *h, int err, const char *msg, int size)
{
	(void)h; (void)err; (void)msg; (void)size;
	busy = 0;
}

static const struct tftp_context tftp_ctx = { t_open, t_close, t_read, t_write, t_error };

/* ---- .data/.bss for lwIP (link_net.ld places them in SDRAM) ---- */
extern uint8_t __net_data_load[], __net_data_start[], __net_data_end[];
extern uint8_t __net_bss_start[], __net_bss_end[];

static void net_crt_init(void)
{
	uint8_t *s = __net_data_load, *d = __net_data_start;

	while (d < __net_data_end)
		*d++ = *s++;
	for (d = __net_bss_start; d < __net_bss_end; d++)
		*d = 0;
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

static void poll_for(uint32_t ms)
{
	uint32_t t = net_now();

	while (net_now() - t < ms) {
		fec_poll(&nif);
		sys_check_timeouts();
		wdt_kick();
	}
}

static int window_main(void *arg)
{
	struct arc_entry *e = arg;
	uint32_t board = cfg32(CFG_BUF, CFG_BOARD_ID), t_up, t_addr = 0, end;
	ip4_addr_t ip, mask, gw;

	net_crt_init();
	icache(1);
	clock_init(BI->sysclk);
	rand_state = board;

	/* Locally administered MAC derived from the board ID: 02:4d:4b ("MK") + id */
	mac[0] = 0x02; mac[1] = 0x4D; mac[2] = 0x4B;
	mac[3] = (uint8_t)(board >> 16); mac[4] = (uint8_t)(board >> 8); mac[5] = (uint8_t)board;

	if (!fec_start(mac, BI->sysclk, NET_LINK_MS)) {
		icache(0);
		return 0;
	}

	lwip_init();
	ip4_addr_set_zero(&ip);
	ip4_addr_set_zero(&mask);
	ip4_addr_set_zero(&gw);
	netif_add(&nif, &ip, &mask, &gw, NULL, fec_netif_init, ethernet_input);
	memcpy(nif.hwaddr, mac, 6);
	netif_set_hostname(&nif, "marquee");
	netif_set_default(&nif);
	netif_set_up(&nif);
	netif_set_link_up(&nif);
	dhcp_start(&nif);
	tftp_init_server(&tftp_ctx);

	t_up = net_now();
	for (;;) {
		uint32_t now = net_now();

		fec_poll(&nif);
		sys_check_timeouts();
		wdt_kick();

		if (!t_addr) {
			if (dhcp_supplied_address(&nif)) {
				t_addr = now;
			} else if (now - t_up > NET_DHCP_MS) {
				dhcp_stop(&nif);
				IP4_ADDR(&ip, 169, 254, 77, 1);
				IP4_ADDR(&mask, 255, 255, 0, 0);
				netif_set_addr(&nif, &ip, &mask, &gw);
				static_ip = 1;
				t_addr = now;
			}
			continue;
		}
		if (boot_ready)
			break;
		/* 5 s with nobody talking; once someone does, stay open a minute past their last request */
		end = t_addr + NET_WINDOW_MS;
		if (last_activity && last_activity + NET_ACTIVE_MS > end)
			end = last_activity + NET_ACTIVE_MS;
		if ((int32_t)(now - end) > 0)
			break;
	}

	poll_for(200);                      /* let the final TFTP ACK go out */
	fec_stop();
	icache(0);
	if (boot_ready) {
		*e = found;
		BOOT_FLAGS &= ~1;               /* not the main-flash upgrade image */
	}
	return boot_ready;
}

int net_window(struct arc_entry *e)
{
	if (BI->sdram_total < 0x01000000)   /* needs the first 8 MB, plus margin */
		return 0;
	return call_on_stack(window_main, e, NET_STACK_TOP);
}
