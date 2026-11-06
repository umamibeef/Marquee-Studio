/* The CPM UARTs (MPC862 RM ch.21): SCC1 is the DB9, SCC2 RS-485 (serial2.h). */
#ifndef UMAMI_SERIAL_H
#define UMAMI_SERIAL_H
#include <stdint.h>

/* Bring up the DB9 as an 8N1 UART at `baud`. */
void serial_init(uint32_t baud);
/* Queue a NUL-terminated string / n bytes for transmit (blocks until the prior buffer drains). */
void serial_puts(const char *s);
void serial_write(const char *s, int n);

/* Non-blocking receive: copy up to `max` received bytes into dst, re-arm the Rx buffer, return the count
 * (0 if nothing has arrived). */
int serial_read(char *dst, int max);

#endif
