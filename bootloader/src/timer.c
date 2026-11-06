/* CPM timer 1 as a millisecond delay. Translated from 0xF8180E50 / 0xF8180E90.
 * TMR1 = 0xF91A: prescaler 250, restart on reference, clock = system clock.
 * At 50 MHz that is 200 kHz, so 200 ticks per millisecond. */
#include "board.h"

void timer_init(void)
{
	TGCR = (TGCR & 0xFF7C) | 0x0004;   /* stop timer 1 */
	TMR1 = 0xF91A;
	TCN1 = 0;
	TRR1 = 0;
	TER1 = 0xFFFF;
	TGCR |= 0x0001;                    /* release timer 1 */
}

void delay_ms(uint32_t ms)
{
	TRR1 = (uint16_t)(ms * 200 - 1);
	TCN1 = 0;
	TER1 = 0xFFFF;
	while (!(TER1 & 0x0002))           /* REF: reached the reference value */
		;
}
