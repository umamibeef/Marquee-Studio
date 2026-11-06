/* MPC862 internal registers and SPRs used by the bootstrap.
 * Offsets are from IMMR (0xFF000000 on this board). Names follow the MPC862 user's manual. */
#ifndef MPC8XX_H
#define MPC8XX_H

#include <stdint.h>

#define IMMR_BASE   0xFF000000u

#define REG32(off)  (*(volatile uint32_t *)(IMMR_BASE + (off)))
#define REG16(off)  (*(volatile uint16_t *)(IMMR_BASE + (off)))
#define REG8(off)   (*(volatile uint8_t  *)(IMMR_BASE + (off)))

/* SIU */
#define SIUMCR      REG32(0x000)
#define SYPCR       REG32(0x004)
#define SWSR        REG16(0x00E)
#define PIPR_HI     REG8 (0x0F0)    /* PCMCIA input pins: board strap byte */

/* Memory controller */
#define BR(n)       REG32(0x100 + 8 * (n))
#define OR(n)       REG32(0x104 + 8 * (n))
#define MAR         REG32(0x164)
#define MCR         REG32(0x168)
#define MAMR        REG32(0x170)
#define MBMR        REG32(0x174)
#define MPTPR       REG16(0x17A)
#define MDR         REG32(0x17C)

/* System integration timers, clocks and reset */
#define PISCR       REG16(0x240)
#define SCCR        REG32(0x280)
#define PLPRCR      REG32(0x284)
#define RSR         REG32(0x288)
#define TBSCRK      REG32(0x380)
#define TBREFAK     REG32(0x384)

/* I2C */
#define I2MOD       REG8 (0x860)
#define I2ADD       REG8 (0x864)
#define I2BRG       REG8 (0x868)
#define I2COM       REG8 (0x86C)
#define I2CER       REG8 (0x870)
#define I2CMR       REG8 (0x874)

/* CPM timers */
#define TGCR        REG16(0x980)
#define TMR1        REG16(0x990)
#define TRR1        REG16(0x994)
#define TCN1        REG16(0x99C)
#define TER1        REG16(0x9B0)
#define CPCR        REG16(0x9C0)

/* CPM timer 2: free-running millisecond clock for the network window */
#define TMR2        REG16(0x992)
#define TRR2        REG16(0x996)
#define TCN2        REG16(0x99E)
#define TER2        REG16(0x9B2)

/* Baud-rate generators BRG1..BRG4 */
#define BRGC(n)     REG32(0x9F0 + 4 * ((n) - 1))

/* Serial interface routing: SMC clock source (SIMODE), SCC clock source (SICR) */
#define SIMODE      REG32(0xAE0)
#define SICR        REG32(0xAEC)

/* SCC1 (UART mode) */
#define GSMRL1      REG32(0xA00)
#define GSMRH1      REG32(0xA04)
#define PSMR1       REG16(0xA08)
#define DSR1        REG16(0xA0E)
#define SCCE1       REG16(0xA10)
#define SCCM1       REG16(0xA14)

/* SMC1 (UART mode) */
#define SMCMR1      REG16(0xA82)
#define SMCE1       REG8 (0xA86)
#define SMCM1       REG8 (0xA8A)

/* Ports */
#define PADIR       REG16(0x950)
#define PAPAR       REG16(0x952)
#define PADAT       REG16(0x956)
#define PCDAT       REG16(0x966)
#define PIPR        REG32(0x0F0)
#define PDDIR       REG16(0x970)
#define PDPAR       REG16(0x972)
#define PDDAT       REG16(0x976)
#define PBDIR       REG32(0xAB8)
#define PBPAR       REG32(0xABC)
#define PBODR       REG16(0xAC2)
#define PBDAT       REG32(0xAC4)

/* Fast Ethernet Controller (MPC860T/862), IMMR + 0xE00 */
#define FEC_ADDR_LOW      REG32(0xE00)
#define FEC_ADDR_HIGH     REG32(0xE04)
#define FEC_HASH_HIGH     REG32(0xE08)
#define FEC_HASH_LOW      REG32(0xE0C)
#define FEC_R_DES_START   REG32(0xE10)
#define FEC_X_DES_START   REG32(0xE14)
#define FEC_R_BUFF_SIZE   REG32(0xE18)
#define FEC_ECNTRL        REG32(0xE40)
#define FEC_IEVENT        REG32(0xE44)
#define FEC_IMASK         REG32(0xE48)
#define FEC_IVEC          REG32(0xE4C)
#define FEC_R_DES_ACTIVE  REG32(0xE50)
#define FEC_X_DES_ACTIVE  REG32(0xE54)
#define FEC_MII_DATA      REG32(0xE80)
#define FEC_MII_SPEED     REG32(0xE84)
#define FEC_R_BOUND       REG32(0xECC)
#define FEC_R_FSTART      REG32(0xED0)
#define FEC_X_WMRK        REG32(0xEE4)
#define FEC_X_FSTART      REG32(0xEEC)
#define FEC_FUN_CODE      REG32(0xF34)
#define FEC_R_CNTRL       REG32(0xF44)
#define FEC_R_HASH        REG32(0xF48)
#define FEC_X_CNTRL       REG32(0xF84)

/* Dual-port RAM */
#define DPRAM(off)  (IMMR_BASE + 0x2000u + (off))

/* SPR numbers (MPC8xx specific) */
#define SPR_DER     149
#define SPR_IMMR    638
#define SPR_ICTRL   158
#define SPR_IC_CST  560
#define SPR_DC_CST  568
#define SPR_DC_ADR  569
#define SPR_MI_CTR  784
#define SPR_MI_EPN  787
#define SPR_MI_TWC  789
#define SPR_MI_RPN  790
#define SPR_MD_CTR  792
#define SPR_M_CASID 793
#define SPR_MD_EPN  795
#define SPR_M_TWB   796
#define SPR_MD_TWC  797
#define SPR_MD_RPN  798

#define mtspr(spr, v) __asm__ volatile("mtspr " #spr ", %0" :: "r"((uint32_t)(v)) : "memory")
#define MTSPR(spr, v) mtspr(spr, v)
#define MFSPR(spr) mfspr(spr)
#define mfspr(spr) ({ uint32_t _v; __asm__ volatile("mfspr %0, " #spr : "=r"(_v)); _v; })

static inline uint32_t mfmsr(void)
{
	uint32_t v;
	__asm__ volatile("mfmsr %0" : "=r"(v));
	return v;
}

static inline void mtmsr(uint32_t v)
{
	__asm__ volatile("mtmsr %0" :: "r"(v) : "memory");
}

static inline void isync_sync(void)
{
	__asm__ volatile("isync\n\tsync" ::: "memory");
}

static inline void tlbia(void)
{
	__asm__ volatile("tlbia\n\tisync" ::: "memory");
}

/* Service the software watchdog (SYPCR enables it at boot). */
static inline void wdt_kick(void)
{
	SWSR = 0x556C;
	SWSR = 0xAA39;
}

#endif
