/* What the application core gives the rest of the firmware: the clock and the attract lines. */
#ifndef CORE_H
#define CORE_H
#include <stdint.h>

#define CLOCK_TPS 100u
uint32_t clock_ticks(void);

#define NLINES_ATTRACT 16
struct attract_cfg {
	uint32_t mode[NLINES_ATTRACT];       /* the line is on */
	uint32_t effect[NLINES_ATTRACT];     /* FG << 16 | BG */
	uint32_t outline[NLINES_ATTRACT][2]; /* style, colour << 16 */
	uint8_t  text_set[NLINES_ATTRACT];   /* the line has text of its own */
};
extern struct attract_cfg g_attract;
extern uint8_t g_message_table[256 * 17];

#endif
