/* Neurizon CPU-002 board layout as used by BOOTSTRAP V2.00.
 * All scratch state lives in the CPM dual-port RAM, because SDRAM is not usable until the
 * memory controller is programmed and the boot flash is read-only. */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include "mpc8xx.h"

/* ---- DPRAM map (absolute addresses) ---- */
#define CFG_BUF     DPRAM(0x040)   /* 0xFF002040: 256 bytes, board config EEPROM (I2C 0xA0) */
#define UPM_BUF     DPRAM(0x140)   /* 0xFF002140: 256 bytes, UPM table EEPROM (I2C 0xA2)    */
#define BOOTINFO    DPRAM(0x240)   /* 0xFF002240: struct boot_info                          */
#define SPD_BUF     DPRAM(0x298)   /* 0xFF002298: 256 bytes, SDRAM SPD EEPROM (I2C 0xA2)    */
#define BANK_LIST   DPRAM(0x398)   /* 0xFF002398: 17 words, SDRAM bank bases, -1 terminated */
#define TLBCNT_A    DPRAM(0x3E0)   /* 0xFF0023E0: TLB fill counters (mmu_final)             */
#define TLBCNT_B    DPRAM(0x3F4)   /* 0xFF0023F4: TLB fill counters (mmu_final_fixed)       */
#define I2C_RXBD    DPRAM(0x408)   /* 0xFF002408 */
#define I2C_TXBD0   DPRAM(0x410)   /* 0xFF002410 */
#define I2C_TXBD1   DPRAM(0x418)   /* 0xFF002418 */
#define I2C_TXBUF   DPRAM(0x420)   /* 0xFF002420: address/offset/address|R bytes            */
#define TLBCNT_C    DPRAM(0x428)   /* 0xFF002428: TLB fill counters (mmu_init_spd)          */
#define I2C_PRAM    DPRAM(0x1C80)  /* 0xFF003C80: I2C parameter RAM                         */

/* The original uses 0xFF002040 as its stack top (64 bytes, enough for the hand-written
 * assembly). C needs more, so the stack sits in unused DPRAM above the BDs instead. */
#define STACK_TOP   DPRAM(0xC00)   /* 0xFF002C00 */

/* ---- Memory map chosen by the bootstrap ---- */
#define BOOT_BASE   0xF8180000u    /* CS0: this flash, 512 KB */
#define BOOT_SIZE   0x00080000u
#define NVRAM_BASE  0xF8780000u    /* CS1: battery-backed RAM */
#define FLASH1_BASE 0x50000000u    /* CS4 (fixed) / CS6 (SPD): main flash */
#define FLASH2_BASE 0x58000000u
#define CS7_BASE    0x60000000u
#define APP_LOAD    0x00000000u    /* the app runs from SDRAM address 0 */
#define APP_STAGE   0x00300000u    /* compressed images are staged here first */

/* Boot-attempt counter in NVRAM: [0] = tries of the upgrade image (0xFF = disabled),
 * [1] bit 0 = "booted the upgrade image". The application is expected to clear [0]. */
#define BOOT_TRIES  (*(volatile uint8_t *)(NVRAM_BASE + 0x7FF0))
#define BOOT_FLAGS  (*(volatile uint8_t *)(NVRAM_BASE + 0x7FF1))

#ifdef DUMP                                /* dbg.c: the debug record in NVRAM (include/dbg.h) */
#include "dbg.h"
void dbg_boot_start(void);
void dbg_step(uint16_t step, uint32_t value);
uint8_t dbg_options(void);
void dbg_set_options(uint8_t v);
void dbg_clear_crash(void);
uint8_t dbg_s1_tries(void);
void dbg_set_s1_tries(uint8_t v);
void dbg_flush(volatile const void *p, uint32_t n);
void dbg_vectors_on(void);
void dbg_vectors_off(void);
#define DBG_STEP(s, v) dbg_step((s), (v))
#else
#define DBG_STEP(s, v) ((void)0)
#endif

/* ---- Boot info block at 0xFF002240 (passed to the app at RAM 0x14..0x98) ---- */
struct boot_info {
	uint32_t sysclk;        /* 0x00 system clock, Hz */
	uint32_t sdram_total;   /* 0x04 */
	uint32_t bank_size;     /* 0x08 */
	uint32_t *bank_list;    /* 0x0C -> BANK_LIST */
	uint32_t flash1_size;   /* 0x10 */
	uint32_t flash1_base;   /* 0x14 */
	uint32_t _18;
	uint32_t flash2_size;   /* 0x1C */
	uint32_t flash2_base;   /* 0x20 */
	uint32_t _24;
	uint32_t cs7_size;      /* 0x28 */
	uint32_t cs7_base;      /* 0x2C */
	uint32_t cs7_virt;      /* 0x30 */
	uint32_t boot_size;     /* 0x34 */
	uint32_t boot_base;     /* 0x38 */
	uint32_t boot_virt;     /* 0x3C */
	uint32_t nvram_size;    /* 0x40 */
	uint32_t nvram_base;    /* 0x44 */
	uint32_t nvram_virt;    /* 0x48 */
	uint32_t mapped_ram;    /* 0x4C bytes of SDRAM mapped by the MMU setup */
	uint32_t flash1_total;  /* 0x50 */
	uint32_t flash2_total;  /* 0x54 */
};

_Static_assert(sizeof(struct boot_info) == 0x58, "boot_info layout");

#define BI ((volatile struct boot_info *)BOOTINFO)

/* ---- Board config EEPROM (I2C 0xA0), byte offsets ---- */
#define CFG_BOARD_ID    0x00   /* u32, copied to RAM 0x98 (the app's X<hex>.lfs name) */
#define CFG_MEM_TYPE    0x04   /* u8: 1 = SDRAM described by SPD                      */
#define CFG_MEM_SUB     0x05   /* u8: must be < 0x50 for the SPD path                  */
#define CFG_SCCR        0x06   /* u16, SCCR = value << 11                              */
#define CFG_SYSCLK      0x08   /* u32 wanted system clock, Hz                          */
#define CFG_OSCCLK      0x0C   /* u32 oscillator, Hz                                   */
#define CFG_OPTIONS     0x10   /* u32 option bits                                      */
#define CFG_SDRAM_OR    0x1A   /* u16, OR2/OR3 = 0xF8000000 | value                    */
#define CFG_OR0         0x1C   /* u32 */
#define CFG_OR1         0x20   /* u32 (NVRAM) */
#define CFG_OR4         0x24   /* u32 (flash bank 1, 0 = absent) */
#define CFG_OR5         0x28   /* u32 (flash bank 2, 0 = absent) */
#define CFG_MCR_A       0x2C   /* u32 UPM run command 1 (the next u16 overlaps its low half) */
#define CFG_MCR_B       0x2E   /* u16 */
#define CFG_MAR         0x30   /* u16 */
#define CFG_MCR_C       0x32   /* u16 */
#define CFG_MPTPR       0x34   /* u8  */
#define CFG_SDRAM_SIZE  0x35   /* u8: 0 = 64 MB, 1 = 128 MB, else 256 MB */
#define CFG_MAMR_INIT   0x36   /* u16 hi, u16 lo */
#define CFG_MAMR_RUN    0x3A   /* u16 hi, u16 lo */
#define CFG_SUM         0x3E   /* u8 sum of bytes 0..0x3D */
#define CFG_XOR         0x3F   /* u8 xor of bytes 0..0x3D */

#define OPT_CS7_ALWAYS  0x80000000u
#define OPT_NVRAM_MASK  0x60000000u
#define OPT_UPM_40MHZ   0x00200000u

static inline uint8_t  cfg8 (uintptr_t b, unsigned o) { return *(volatile uint8_t  *)(b + o); }
static inline uint16_t cfg16(uintptr_t b, unsigned o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t cfg32(uintptr_t b, unsigned o) { return *(volatile uint32_t *)(b + o); }

/* ---- Prototypes ---- */
void     timer_init(void);
void     delay_ms(uint32_t ms);

void     i2c_init(void);
void     i2c_stop(void);
int      i2c_read(uint8_t dev, uintptr_t buf, uint32_t len);
int      i2c_write(uint8_t dev, uintptr_t buf, int len);
void     eeprom_sum(uintptr_t buf, uint32_t len, uint8_t *sum, uint8_t *xr);

void     memmap_from_straps(uintptr_t cfg);
int      sdram_from_spd(uintptr_t cfg);

int      mmu_init_spd(void);
int      mmu_final(uint32_t *table, uint32_t table_len);
int      mmu_final_fixed(uint32_t *table, uint32_t table_len);

int      load_app_fixed(void);
int      load_app_spd(void);
struct arc_entry;
int      load_image(const struct arc_entry *e);

#ifdef NET
int      net_window(struct arc_entry *e);
#endif
#ifdef DUMP
void     dump_window(void);
#endif
#ifdef PATCH
int      apply_patches(void);
#endif

void     boot_main(void);
void     boot_handoff(int status) __attribute__((noreturn));

#endif
