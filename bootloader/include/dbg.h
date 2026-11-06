/* The debug record in NVRAM: what the last boots did and where the last crash was, kept across resets and power
 * cycles (the NVRAM is battery-backed). Shared by our boot loader (DUMP builds) and the firmware extensions
 * (firmware/ext), read back through the serial window (region nvram, tools/serdump.py --boots), the console
 * ("boots") and Umami Marquee Studio. Self-contained: no board.h.
 *
 * It lives in the NVRAM's top 0x410 bytes, which the firmware's settings chain never uses (nvram_erase keeps them;
 * the original only uses 0x7FF0/0x7FF1 there), at NVRAM + 0x7C00:
 *   +0x000  header: magic, boot number, options
 *   +0x010  the last 8 boots, boots[seq & 7]: number, reset cause (RSR), last step reached and a value
 *   +0x100  the last crash: who caught it, vector, SRR0/SRR1/DAR/DSISR/LR/CR, r0..r31, the step it happened in
 * The firmware only writes here when the magic is set, which only our boot loader does, so a stock boot loader
 * leaves the NVRAM exactly as the original would. */
#ifndef DBG_H
#define DBG_H
#include <stdint.h>

#define DBG_OFFSET      0x7C00u
#define DBG_MAGIC       0x4D514442u  /* "MQDB" */
#define DBG_CRASH_MAGIC 0x4D514352u  /* "MQCR" */
#define DBG_BOOTS       8

/* options (window OPTION request) */
#define DBG_OPT_SLOT_DIRECT 0x01     /* boot the update slot in place (main flash as boot volume), not via the stage */

/* steps: boot loader 0x01..0x3F, firmware 0x40..0x7F */
#define STEP_BOOT          0x01      /* NVRAM chip select up, the record started */
#define STEP_SDRAM         0x02      /* SDRAM up, exception stubs in place */
#define STEP_WINDOW_DONE   0x03      /* the serial window closed */
#define STEP_STAGE1        0x04      /* stage 1 running (from the main flash's loader sector); value: its address */
#define STEP_S1_SKIPPED    0x05      /* stage 1 not used; value: 1 recovery window, 2 bad image, 3 tries used up */
#define STEP_SLOT_FOUND    0x10      /* value: the slot address */
#define STEP_SLOT_TRY      0x11      /* value: the try number */
#define STEP_SLOT_COPIED   0x12      /* copied to the stage; value: stage address */
#define STEP_SLOT_DIRECT   0x13      /* booting the slot in place (DBG_OPT_SLOT_DIRECT) */
#define STEP_CHIP          0x18      /* booting the boot flash's image */
#define STEP_STAGED        0x19      /* RUN: the staged image */
#define STEP_LOADING       0x20      /* value: the entry's data address */
#define STEP_LOADED        0x21
#define STEP_MMU           0x22
#define STEP_HANDOFF       0x23      /* value: the boot volume (RAM 0x2C) */
#define STEP_FW_START      0x40      /* firmware: ext_boot_card */
#define STEP_FW_VECTORS    0x41      /* the firmware's exception table (crash reports from here on) */
#define STEP_FW_VOLUMES    0x42      /* main_task started (the boot volume is read from here) */
#define STEP_FW_LAYOUT     0x43      /* main_task: ram_layout_init_maybe done (main flash mapped) */
#define STEP_FW_HEALTHY    0x4F      /* 1200 panel frames */

/* crash sources */
#define DBG_SRC_BOOTLOADER 1         /* an exception in the boot loader (its stubs in SDRAM) */
#define DBG_SRC_TLB_HANG   2         /* the firmware's DTLB miss handler found no mapping (it would spin) */
#define DBG_SRC_FIRMWARE   3         /* firmware/ext/crash.c */

struct dbg_boot {
	uint32_t seq;
	uint32_t rsr;                /* reset status at the start of this boot (then cleared) */
	uint16_t step;               /* the last step reached */
	uint16_t pad;
	uint32_t value;
};

struct dbg_crash {                   /* the offsets are used by assembly: keep them */
	uint32_t magic;              /* +0x00 DBG_CRASH_MAGIC when valid */
	uint32_t seq;                /* +0x04 the boot it happened in */
	uint32_t source;             /* +0x08 */
	uint32_t vector;             /* +0x0C exception offset (0x200 machine check, 0x300 DSI, 0x700 program, ...) */
	uint32_t srr0, srr1;         /* +0x10 */
	uint32_t dar, dsisr;         /* +0x18 (TLB hang: dar = MD_EPN, the address that missed) */
	uint32_t lr, cr;             /* +0x20 */
	uint32_t step, value;        /* +0x28 the boot's last step when it happened */
	uint32_t gpr[32];            /* +0x30 */
};

struct dbg_area {
	uint32_t magic;
	uint32_t seq;                /* boot number, counts up */
	uint8_t options;
	uint8_t s1_tries;            /* stage 1 boots not yet handed off (serdump.h, two stages) */
	uint8_t pad[6];
	struct dbg_boot boots[DBG_BOOTS];    /* +0x010 */
	uint8_t reserved[0x100 - 0x10 - DBG_BOOTS * 16];
	struct dbg_crash crash;              /* +0x100 */
};

#define DBG_CRASH_OFFSET (DBG_OFFSET + 0x100u)
#define DBG_GPR_OFFSET   0x30u
#define DBG_SIZE         ((uint32_t)sizeof(struct dbg_area))

static inline volatile struct dbg_boot *dbg_cur(volatile struct dbg_area *d)
{
	return &d->boots[d->seq & (DBG_BOOTS - 1)];
}

static inline void dbg_mark(volatile struct dbg_area *d, uint16_t step, uint32_t value)
{
	if (d->magic == DBG_MAGIC) {
		volatile struct dbg_boot *b = dbg_cur(d);

		b->step = step;
		b->value = value;
	}
}
#endif
