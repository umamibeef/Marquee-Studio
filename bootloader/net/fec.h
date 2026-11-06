/* MPC862 Fast Ethernet Controller driver (polled) for lwIP. */
#ifndef FEC_H
#define FEC_H

#include <stdint.h>
#include "lwip/netif.h"

/* Bring up pins, the PHY and the controller. Waits up to link_ms for a link.
 * Returns 1 with a link, 0 if there is no PHY or no link (the FEC is then shut down again). */
int  fec_start(const uint8_t mac[6], uint32_t sysclk, uint32_t link_ms);
/* Stop the controller and give Port D back exactly as it was before fec_start(). */
void fec_stop(void);
/* Hand every received frame to the netif. */
void fec_poll(struct netif *nif);
/* netif init callback for netif_add(). */
err_t fec_netif_init(struct netif *nif);

/* What fec_start() found, for the info file. */
struct fec_status {
	int      phy_addr;      /* -1 if none */
	uint32_t phy_id;        /* reg2 << 16 | reg3 */
	int      speed100;
	int      full_duplex;
	uint32_t rx_frames, rx_errors, tx_frames, tx_busy;
};
extern struct fec_status fec_stat;

#endif
