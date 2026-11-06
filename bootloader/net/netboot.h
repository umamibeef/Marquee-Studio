/* Network window: Ethernet + lwIP during boot, for uploading an app to RAM and for
 * reading out the flash. Built only with `make NET=1`. */
#ifndef NETBOOT_H
#define NETBOOT_H

#include <stdint.h>
#include "archive.h"

/* SDRAM used by the window. It lies inside the first 8 MB (present on every memory
 * configuration) and clear of what the loader touches afterwards: the app at 0..~0xBE000
 * and the NRV2D staging copy at 0x300000. */
#define NET_UPLOAD_BASE   0x00400000u   /* TFTP uploads land here                */
#define NET_UPLOAD_MAX    0x00280000u   /* 2.5 MB                                */
#define NET_DATA_BASE     0x00680000u   /* lwIP .data/.bss (link_net.ld)         */
#define NET_FEC_AREA      0x00700000u   /* FEC rings and buffers                 */
#define NET_STACK_TOP     0x00800000u   /* C stack while the window runs         */

/* Timing */
#define NET_LINK_MS       2500          /* wait this long for a cable/link       */
#define NET_DHCP_MS       3000          /* then fall back to the static address  */
#define NET_WINDOW_MS     5000          /* listen this long once addressed       */
#define NET_ACTIVE_MS     60000         /* stay open this long after each request */

/* Static fallback address when no DHCP server answers: link-local, so a laptop on a
 * direct cable (which also falls back to 169.254/16) can reach it. */
#define NET_STATIC_IP     "169.254.77.1"

/* Run the window. Returns 1 and fills *e when an uploaded image should be booted. */
int  net_window(struct arc_entry *e);

/* Milliseconds since net_window() started (CPM timer 2). */
uint32_t net_now(void);

/* Load a found entry: copy (and decompress) it to address 0 (loader.c). */
int  load_image(const struct arc_entry *e);

/* Run fn(arg) on another stack (start.S). */
int  call_on_stack(int (*fn)(void *), void *arg, uint32_t stack_top);

#endif
