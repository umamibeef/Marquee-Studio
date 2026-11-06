/* Serial window over the DB9, built with `make DUMP=1`: read-only extraction of the board, plus "run from RAM":
 * the host writes a whole boot-flash image into a staging area at the top of SDRAM and the boot goes on from it
 * as if it were the boot flash. Nothing is ever written to flash, NVRAM or an EEPROM.
 * The host side is ../tools/serdump.py; test/serdumpsim.py checks both in emulation.
 *
 * Protocol (all fields big-endian):
 *   1. After reset the window listens DUMP_LISTEN_MS at 115200 8N1 for DUMP_MAGIC.
 *      Nothing: it closes and the boot carries on as usual.
 *   2. Magic seen: the sign answers with one text line "MQDUMP READY ...\r\n" and serves
 *      frames until a BOOT request or DUMP_IDLE_MS without a valid request.
 *   3. Frames, both directions:
 *        'M' 'Q' type tag len16 payload[len] crc32
 *      crc32 is the zlib CRC-32 of type..payload as sent. A frame with a bad CRC is dropped;
 *      the host simply asks again (every request is idempotent). The reply echoes the tag.
 *      In requests (host -> sign) every payload byte is XORed with the tag. The host picks a
 *      tag that keeps 0x5A out of all the bytes it sends: 0x5A is the sync byte of the app's
 *      DB9 download frames, so even a stray request can never look like the start of one.
 *   4. Requests and replies:
 *        HELLO  -                          -> info text (board, memory map, regions, NVRAM flags)
 *        READ   region u8, 0[3], off u32, len u32 (<= DUMP_BLOCK_MAX)
 *                                          -> the same 12 bytes, then the data
 *        CRC    same 12 bytes (any length inside the region)
 *                                          -> the same 12 bytes, then crc32 u32 of that range
 *        SPEED  baud u32                   -> status u8, 0[3], actual baud u32, at the old speed;
 *                                             then the sign switches. If no valid frame comes in
 *                                             within DUMP_FALLBACK_MS it goes back to 115200.
 *        PING   -                          -> empty (keep-alive, speed check)
 *        BOOT   -                          -> empty, then the window closes and the boot goes on
 *        WRITE  region u8 (STAGE only), 0[3], off u32, data[1..DUMP_WRITE_MAX]
 *                                          -> region, 0[3], off u32, len u32, crc32 u32 of the data as stored
 *        RUN    len u32, crc32 u32         -> empty, if the first len bytes of STAGE have that CRC and hold a
 *                                             bootable archive entry; then the window closes and the app is
 *                                             loaded from STAGE, which it then uses as its boot flash (RAM
 *                                             0x2C/0x30) and never sees as RAM (RAM 0x14 is cut by STAGE_SIZE).
 *                                             Otherwise ERROR E_CHECK, and the window stays open.
 *        INSTALL len u32, crc u32         -> empty, after the staged image (checked as for RUN) is in the update slot
 *                                             (the last SLOT_SIZE bytes of the main flash: erase, program, CRC of
 *                                             the flash) and the NVRAM boot count is 0, so the next boot tries it.
 *                                             ERROR E_CHECK (image) or E_FLASH (erase/program/verify).
 *        UNINSTALL -                       -> empty, after the slot's first sector is erased: the boot flash boots.
 *      Errors come back as ERROR: code u8, request type u8, 0[2].
 *   Reads are limited to the regions below; writes go to STAGE only, and INSTALL to the update slot only. */
#ifndef SERDUMP_H
#define SERDUMP_H

#define DUMP_MAGIC        "MQ?DUMP\r"    /* no 0x5A: never a sync byte of the app's DB9 frames */
#define DUMP_MAGIC_LEN    8
#define DUMP_BAUD         115200         /* the app's DB9 speed */
#define DUMP_LISTEN_MS    2000           /* wait this long for the magic after reset */
#define DUMP_IDLE_MS      30000          /* then close after this long without a request */
#define DUMP_FALLBACK_MS  3000           /* back to DUMP_BAUD after this long without a frame */
#define DUMP_GAP_MS       100            /* a request with a gap this long is dropped        */
#define DUMP_BLOCK_MAX    0x4000         /* largest READ */
#define DUMP_WRITE_MAX    0x400          /* largest WRITE data */

#define DUMP_SYNC0        'M'
#define DUMP_SYNC1        'Q'
#define DUMP_HDR          6              /* sync, type, tag, len16 */
#define DUMP_FIXED_MAX    16             /* largest request payload other than WRITE */
#define DUMP_REQ_MAX      (8 + DUMP_WRITE_MAX)   /* largest WRITE payload (its data goes straight to the stage) */

/* Request types; a reply is the request type | 0x80 */
#define DUMP_HELLO        0x01
#define DUMP_READ         0x02
#define DUMP_CRC          0x03
#define DUMP_SPEED        0x04
#define DUMP_PING         0x05
#define DUMP_BOOT         0x06
#define DUMP_WRITE        0x07
#define DUMP_RUN          0x08
#define DUMP_INSTALL      0x09
#define DUMP_UNINSTALL    0x0A
#define DUMP_OPTION       0x0B           /* [op, value]: debugging switches (OPT_*); reply [options, tries, flags, 0] */
#define OPT_SET_OPTIONS   1              /* the debug record's options byte (include/dbg.h DBG_OPT_*) */
#define OPT_SET_TRIES     2              /* BOOT_TRIES (0 = try the slot again, 0xFF = never) */
#define OPT_CLEAR_CRASH   3
#define OPT_TEST_FAULT    4              /* after the reply: a program exception (trap), to see the crash record work */
#define OPT_SET_FLAGS     5              /* BOOT_FLAGS */
#define OPT_SET_S1_TRIES  6              /* the stage-1 try counter (0 = try stage 1 again) */
#define DUMP_INSTALL_S1   0x0C           /* like INSTALL, the staged image is a stage-1 boot loader (s1_header) */
#define DUMP_UNINSTALL_S1 0x0D           /* erase the loader sector: the chip's own boot loader from then on */
#define DUMP_INSTALL_BOOT 0x0E           /* write the staged image to the boot flash (the chip): only when not running
                                          * from it, i.e. as stage 1 or a RUN image, never stage 0 in place */

/* Two stages (main.c stage0_try): the chip's boot loader (stage 0) sets up clocks, chip selects and SDRAM, then
 * boots a stage 1 from the loader sector, the 64 KB of the main flash just below the update slot: this same code
 * built with STAGE1=1, linked for SDRAM at S1_LOAD + 0x20, behind this header. Its window, slot boot, debug record,
 * loading and hand-off then replace the chip's, and it is installed over the DB9 (INSTALL_S1). Stage 0 counts the
 * tries (dbg.h s1_tries; stage 1 clears it at the hand-off) and after 2 it uses its own code again. And when a stage
 * 1 is installed, stage 0 first listens DUMP_RECOVERY_MS for DUMP_RECOVERY_MAGIC: then its own window, and no
 * stage 1 this boot (recovery from a stage 1 that boots but whose window is broken). */
#define LOADER_SIZE       0x00010000
#define S1_MAGIC          0x4D515331     /* "MQS1" */
#define S1_LOAD           0x00F00000     /* bank 0 (virtual = physical), clear of the app, APP_STAGE and the stage */
#define S1_HDR            0x20
#define DUMP_RECOVERY_MAGIC "MQ?DUMP0"
#define DUMP_RECOVERY_MS  300
struct s1_header {
	uint32_t magic;                      /* S1_MAGIC */
	uint32_t len;                        /* code bytes after the header */
	uint32_t crc;                        /* CRC-32 (zlib) of the code */
	uint32_t entry;                      /* entry offset from the header (S1_HDR) */
	char version[16];                    /* NUL-padded */
};
uint32_t loader_where(void);             /* mainflash.c: the loader sector, 0 if there is no room */
int loader_install(const uint8_t *src, uint32_t n);
int loader_uninstall(void);
int s1_valid(uintptr_t a);               /* serdump.c: a stage-1 image at a with a good header and CRC */
uint32_t boot_flash_id(void);            /* mainflash.c: the boot-flash chip's manufacturer<<8 | device id */
int boot_flash_install(const uint8_t *src, uint32_t n);   /* write the chip; -1 if refused or on a flash error */
int running_from_bootflash(void);        /* nonzero if this code runs in place from the boot flash (stage 0) */
int dump_window_recovery(void);          /* serdump.c: 1 if the recovery window was opened (and served) */
#define DUMP_ERROR        0xFF

/* ERROR codes */
#define DUMP_E_TYPE       1              /* unknown request */
#define DUMP_E_LEN        2              /* wrong payload length */
#define DUMP_E_REGION     3              /* no such region, or absent on this board */
#define DUMP_E_RANGE      4              /* offset/length outside the region */
#define DUMP_E_I2C        5              /* EEPROM did not answer */
#define DUMP_E_BAUD       6              /* speed not reachable within DUMP_BAUD_TOL_PCT */
#define DUMP_E_CHECK      7              /* RUN: CRC differs, or no bootable entry in the staged image */
#define DUMP_E_WRITE      8              /* WRITE to a region other than STAGE */
#define DUMP_E_FLASH      9              /* INSTALL / UNINSTALL: the main flash did not erase, program or verify */

#define DUMP_BAUD_TOL_PCT 5              /* accept a speed change within this error */

/* Regions (the read allow-list) */
#define DUMP_R_BOOT       1              /* boot flash, CS0                       */
#define DUMP_R_FLASH1     2              /* main flash bank 1                     */
#define DUMP_R_FLASH2     3              /* main flash bank 2                     */
#define DUMP_R_NVRAM      4              /* battery-backed RAM, CS1               */
#define DUMP_R_EE_A0      5              /* board config EEPROM, read live by I2C */
#define DUMP_R_EE_A2      6              /* UPM program / SPD EEPROM, live by I2C */
#define DUMP_R_SDRAM      7              /* first SDRAM bank, untouched so far    */
#define DUMP_R_STAGE      8              /* the last STAGE_SIZE bytes of SDRAM: an image to run */
#define DUMP_R_SLOT       9              /* the update slot: the last SLOT_SIZE bytes of the main flash */
#define DUMP_R_COUNT      10

#define SLOT_FLAG 0x02              /* BOOT_FLAGS: booted from the update slot (via the stage) */
#define SLOT_SIZE         0x00080000     /* one boot flash */
uint32_t slot_where(uint32_t *chip);     /* mainflash.c: the slot's address (0: no main flash), its chip's base */
int slot_install(const uint8_t *src, uint32_t n);
int slot_uninstall(void);

/* Run from RAM: the staging area, and what the boot needs to know about it (DPRAM, after the I2C buffers;
 * cleared at reset). The banks are mapped back to back from virtual 0, so the last STAGE_SIZE bytes of the
 * last bank are the last STAGE_SIZE bytes of the app's RAM. */
#define STAGE_SIZE        0x00080000     /* one boot flash */
#define STAGE_INFO        DPRAM(0x440)
#define STAGE_MAGIC       0x52554E21     /* "RUN!" */
struct stage_info {
	uint32_t magic;                      /* STAGE_MAGIC: boot from the staged image */
	uint32_t phys;                       /* where it is (the MMU is off until the final map) */
	uint32_t virt;                       /* where the app sees it */
	uint32_t len;
};
#define STAGE ((volatile struct stage_info *)STAGE_INFO)
void stage_where(uint32_t *phys, uint32_t *virt);
int load_app_staged(void);

/* mainflash.c: where the last slot erase/program failed (sent with DUMP_E_FLASH) */
struct flash_fail { uint32_t step, addr; uint16_t got, want; };
#define FLASH_FAIL_AT DPRAM(0x450)   /* after STAGE_INFO; the boot loader has no writable data */
#define FLASH_STEP_NONE    1   /* no main flash, or the image is too big */
#define FLASH_STEP_ERASE   2
#define FLASH_STEP_PROGRAM 3
#define FLASH_STEP_BLANK   4   /* a 0xFFFF word was not blank after the erase */
#define FLASH_STEP_VERIFY  5   /* the CRC over the slot did not match */
#define flash_fail (*(volatile struct flash_fail *)FLASH_FAIL_AT)               /* loader.c: 0, or nonzero when the staged image did not load */

/* DPRAM used by the window (above the C stack, which grows down from 0xFF002C00).
 * Every CPM transfer stays inside DPRAM, as the boot's own I2C transfers do: data from
 * flash, NVRAM and SDRAM is copied through the TX bounce buffers by the core. */
#define DUMP_RXBD         DPRAM(0xC00)   /* 16 RX BDs, one character each        */
#define DUMP_TXBD         DPRAM(0xC80)   /* 8 TX BDs                             */
#define DUMP_RXBUF        DPRAM(0xCC0)   /* 16 bytes                             */
#define DUMP_HDRBUF       DPRAM(0xCD0)   /* 32 bytes: frame header + small payload */
#define DUMP_TRLBUF       DPRAM(0xCF0)   /* 16 bytes: CRC trailer                */
#define DUMP_EEBUF        DPRAM(0xD00)   /* 256 bytes: live EEPROM copy          */
#define DUMP_BOUNCE       DPRAM(0xE00)   /* 4 x 128 bytes TX bounce, or the info text */
#define DUMP_RX_RING      16
#define DUMP_TX_RING      8
#define DUMP_BOUNCE_N     4
#define DUMP_BOUNCE_SZ    128
#define DUMP_TEXT_MAX     (DUMP_BOUNCE_N * DUMP_BOUNCE_SZ)

/* Parameter RAM of the two candidate channels */
#define SMC1_PRAM         DPRAM(0x1E80)  /* 0xFF003E80 */
#define SCC1_PRAM         DPRAM(0x1C00)  /* 0xFF003C00 */

#endif
