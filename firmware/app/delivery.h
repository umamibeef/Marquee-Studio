/* Framed delivery (UMFRAME over the DB9 and RS-485) on top of the apps: the clock, data values, USC1 schedules, updates. */
#ifndef DELIVERY_H
#define DELIVERY_H
#include <stdint.h>
void delivery_boot(void);                  /* a USC1 schedule carried in the boot image */
void delivery_rx(int chan, uint8_t c);     /* chan 0 the DB9, 1 RS-485 */
void delivery_refresh_line(unsigned line); /* expand the line's {key} templates: before it starts */
void slot_healthy(void);
#endif
