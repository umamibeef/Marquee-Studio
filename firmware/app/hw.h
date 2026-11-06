/* Board hardware layer for the sign (MPC862, board type 2, E-series panel). Behaviour follows the MPC862 manual and the
 * values the board needs; see hw.c. */
#ifndef HW_H
#define HW_H
#include <stdint.h>

#define IMMR     0xFF000000u
#define HW8(o)   (*(volatile uint8_t  *)(IMMR + (o)))
#define HW16(o)  (*(volatile uint16_t *)(IMMR + (o)))
#define HW32(o)  (*(volatile uint32_t *)(IMMR + (o)))

/* The panel FPGA takes a 240 x 128 RGB555 frame; this sign's LEDs are its top-left 200 x 16. */
#define FRAME_W  240
#define FRAME_H  128

void     wdt_kick(void);
uint32_t brg_clock_hz(void);
uint32_t cpu_clock_hz(void);
void     delay_us(uint32_t us);
void     cache_flush(const void *p, uint32_t n);
void     cpm_command(uint16_t op);

const char *fpga_load(void);                         /* 0 when DONE, else why not */
void     panel_init(void);
void     panel_present(const uint16_t *px, int w, int h);   /* wait for the panel, then send a w x h image */
uint32_t panel_frames_sent(void);
int      panel_ready(void);                          /* the panel has taken the last frame */
void     panel_blank(void);

void     hw_reset(void);                             /* panel off, then the watchdog restarts the board */
void     mmu_map_main_flash(void);                   /* data-side TLB entries for the main flash */
void     time_init(void);
uint64_t time_ticks(void);                           /* the time base */
uint32_t time_hz(void);

#endif
