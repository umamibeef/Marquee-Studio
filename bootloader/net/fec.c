/* MPC862 Fast Ethernet Controller driver, polled, for lwIP.
 *
 * Register use follows the MPC860T/862 FEC as used by the RTEMS and U-Boot MPC8xx drivers:
 * MII pins on Port D (PD3..PD15), buffer descriptors in SDRAM, MDIO through MII_DATA.
 * The board's PHY is an Intel LXT972A (ID 0x0013 / 0x78Ex); its MDIO address is found by
 * scanning, and the speed/duplex result is read from the standard autonegotiation registers.
 *
 * The network window runs before the bootloader turns on the data cache, so the descriptor
 * rings and buffers are uncached and need no cache maintenance. */
#include "board.h"
#include "netboot.h"
#include "fec.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "libc/string.h"

#define RX_RING   16
#define TX_RING   4
#define BUF_SIZE  1536        /* multiple of 16, >= 1518 */

/* Buffer descriptor status bits (same layout as the CPM BDs) */
#define BD_E      0x8000      /* RX: empty (owned by the FEC) */
#define BD_R      0x8000      /* TX: ready (owned by the FEC) */
#define BD_W      0x2000      /* wrap: last BD in the ring */
#define BD_L      0x0800      /* last buffer of the frame */
#define BD_TC     0x0400      /* TX: append CRC */
#define BD_RX_ERR 0x0037      /* LG | NO | SH | CR | OV | TR */

#define ECNTRL_RESET   0x00000001
#define ECNTRL_ETHER   0x00000002
#define ECNTRL_PINMUX  0x00000004
#define RCNTRL_DRT     0x00000002
#define RCNTRL_MII     0x00000004
#define XCNTRL_FDEN    0x00000004
#define IEVENT_MII     0x00800000
#define DES_ACTIVE     0x01000000

struct bd {
	volatile uint16_t status;
	volatile uint16_t len;
	volatile uint32_t buf;
};

/* Fixed SDRAM area (see netboot.h) */
#define RXBD      ((struct bd *)(NET_FEC_AREA))
#define TXBD      ((struct bd *)(NET_FEC_AREA + 0x100))
#define RXBUF(i)  (NET_FEC_AREA + 0x200 + (uint32_t)(i) * BUF_SIZE)
#define TXBUF(i)  (NET_FEC_AREA + 0x200 + (uint32_t)(RX_RING + (i)) * BUF_SIZE)

struct fec_status fec_stat;
static unsigned rx_next, tx_next;
static uint16_t saved_pdpar, saved_pddir;

/* ---- MDIO ---- */
static int mii_wait(void)
{
	uint32_t t = net_now();

	while (!(FEC_IEVENT & IEVENT_MII))
		if (net_now() - t > 10)
			return -1;
	FEC_IEVENT = IEVENT_MII;
	return 0;
}

static int mii_read(int phy, int reg)
{
	FEC_IEVENT = IEVENT_MII;
	FEC_MII_DATA = 0x60020000u | (uint32_t)phy << 23 | (uint32_t)reg << 18;
	if (mii_wait())
		return -1;
	return (int)(FEC_MII_DATA & 0xFFFF);
}

static void mii_write(int phy, int reg, uint16_t v)
{
	FEC_IEVENT = IEVENT_MII;
	FEC_MII_DATA = 0x50020000u | (uint32_t)phy << 23 | (uint32_t)reg << 18 | v;
	mii_wait();
}

static int phy_find(void)
{
	int a;

	for (a = 0; a < 32; a++) {
		int id2 = mii_read(a, 2), id3 = mii_read(a, 3);

		if (id2 > 0 && id2 != 0xFFFF && id3 >= 0) {
			fec_stat.phy_id = (uint32_t)id2 << 16 | (uint32_t)id3;
			return a;
		}
	}
	return -1;
}

/* Reset, autonegotiate, wait for a link. Returns 1 with a link. */
static int phy_link(int phy, uint32_t link_ms)
{
	uint32_t t0 = net_now();
	int bmsr, common;

	mii_write(phy, 0, 0x8000);                       /* BMCR: reset */
	while ((mii_read(phy, 0) & 0x8000) && net_now() - t0 < 500)
		wdt_kick();
	mii_write(phy, 4, 0x01E1);                       /* advertise 10/100, half/full */
	mii_write(phy, 0, 0x1200);                       /* autoneg enable + restart */

	for (;;) {
		wdt_kick();
		bmsr = mii_read(phy, 1);
		if (bmsr > 0 && (bmsr & 0x0024) == 0x0024)   /* autoneg done, link up */
			break;
		if (net_now() - t0 > link_ms)
			return 0;
	}
	common = mii_read(phy, 4) & mii_read(phy, 5);
	fec_stat.speed100 = (common & 0x0380) != 0;
	fec_stat.full_duplex = (common & 0x0100) || (!(common & 0x0080) && (common & 0x0040));
	return 1;
}

static void rings_init(void)
{
	unsigned i;

	for (i = 0; i < RX_RING; i++) {
		RXBD[i].buf = RXBUF(i);
		RXBD[i].len = 0;
		RXBD[i].status = BD_E | (i == RX_RING - 1 ? BD_W : 0);
	}
	for (i = 0; i < TX_RING; i++) {
		TXBD[i].buf = TXBUF(i);
		TXBD[i].len = 0;
		TXBD[i].status = i == TX_RING - 1 ? BD_W : 0;
	}
	rx_next = 0;
	tx_next = 0;
}

int fec_start(const uint8_t mac[6], uint32_t sysclk, uint32_t link_ms)
{
	uint32_t t;

	memset(&fec_stat, 0, sizeof fec_stat);
	fec_stat.phy_addr = -1;

	/* Port D carries the MII signals. Older silicon (mask < 0x0501) uses a different
	 * direction pattern, as in the U-Boot MPC860T driver. */
	saved_pdpar = PDPAR;
	saved_pddir = PDDIR;
	PDPAR = 0x1FFF;
	PDDIR = (MFSPR(SPR_IMMR) & 0xFFFF) < 0x0501 ? 0x1C58 : 0x1FFF;

	FEC_ECNTRL = ECNTRL_RESET;
	t = net_now();
	while ((FEC_ECNTRL & ECNTRL_RESET) && net_now() - t < 10)
		;
	FEC_IEVENT = 0xFFC00000;
	FEC_IMASK = 0;
	FEC_MII_SPEED = ((sysclk + 4999999) / 5000000) << 1;    /* MDC <= 2.5 MHz */
	FEC_ECNTRL = ECNTRL_PINMUX;                             /* MII up, MAC still off */

	fec_stat.phy_addr = phy_find();
	if (fec_stat.phy_addr < 0 || !phy_link(fec_stat.phy_addr, link_ms)) {
		fec_stop();
		return 0;
	}

	rings_init();
	FEC_ADDR_LOW = (uint32_t)mac[0] << 24 | (uint32_t)mac[1] << 16 | (uint32_t)mac[2] << 8 | mac[3];
	FEC_ADDR_HIGH = (uint32_t)mac[4] << 24 | (uint32_t)mac[5] << 16;
	FEC_HASH_HIGH = 0;
	FEC_HASH_LOW = 0;
	FEC_R_BUFF_SIZE = BUF_SIZE;
	FEC_R_HASH = 1518;                                       /* max frame length */
	FEC_R_DES_START = (uint32_t)RXBD;
	FEC_X_DES_START = (uint32_t)TXBD;
	FEC_FUN_CODE = 0x78000000;
	FEC_R_CNTRL = RCNTRL_MII | (fec_stat.full_duplex ? 0 : RCNTRL_DRT);
	FEC_X_CNTRL = fec_stat.full_duplex ? XCNTRL_FDEN : 0;
	FEC_ECNTRL = ECNTRL_PINMUX | ECNTRL_ETHER;
	FEC_R_DES_ACTIVE = DES_ACTIVE;
	return 1;
}

void fec_stop(void)
{
	FEC_ECNTRL = ECNTRL_RESET;
	PDPAR = saved_pdpar;
	PDDIR = saved_pddir;
}

void fec_poll(struct netif *nif)
{
	while (!(RXBD[rx_next].status & BD_E)) {
		struct bd *bd = &RXBD[rx_next];
		uint16_t st = bd->status;

		if ((st & BD_RX_ERR) || !(st & BD_L) || bd->len < 18) {
			fec_stat.rx_errors++;
		} else {
			uint16_t len = (uint16_t)(bd->len - 4);          /* drop the CRC */
			struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);

			if (p) {
				pbuf_take(p, (const void *)bd->buf, len);
				if (nif->input(p, nif) != ERR_OK)
					pbuf_free(p);
				fec_stat.rx_frames++;
			}
		}
		bd->len = 0;
		bd->status = BD_E | (rx_next == RX_RING - 1 ? BD_W : 0);
		FEC_R_DES_ACTIVE = DES_ACTIVE;
		rx_next = (rx_next + 1) % RX_RING;
	}
}

static err_t fec_output(struct netif *nif, struct pbuf *p)
{
	struct bd *bd = &TXBD[tx_next];
	uint32_t t = net_now();
	uint16_t len;

	(void)nif;
	while (bd->status & BD_R) {                             /* ring full: wait briefly */
		if (net_now() - t > 20) {
			fec_stat.tx_busy++;
			return ERR_IF;
		}
	}
	len = (uint16_t)pbuf_copy_partial(p, (void *)bd->buf, BUF_SIZE, 0);
	if (len < 60) {                                         /* pad to the Ethernet minimum */
		memset((uint8_t *)bd->buf + len, 0, 60u - len);
		len = 60;
	}
	bd->len = len;
	bd->status = BD_R | BD_L | BD_TC | (tx_next == TX_RING - 1 ? BD_W : 0);
	FEC_X_DES_ACTIVE = DES_ACTIVE;
	tx_next = (tx_next + 1) % TX_RING;
	fec_stat.tx_frames++;
	return ERR_OK;
}

err_t fec_netif_init(struct netif *nif)
{
	nif->name[0] = 'f';
	nif->name[1] = 'e';
	nif->output = etharp_output;
	nif->linkoutput = fec_output;
	nif->mtu = 1500;
	nif->hwaddr_len = 6;
	nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
	return ERR_OK;
}
