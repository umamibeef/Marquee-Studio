/* From-source CPM SCC2 UART (RS-485 delivery channel). See serial2.c. */
#ifndef SERIAL2_H
#define SERIAL2_H
#include <stdint.h>
void scc_init(uint32_t baud);
void scc_write(const char *s, int n);
void scc_puts(const char *s);
int  scc_read(char *dst, int max);
#endif
