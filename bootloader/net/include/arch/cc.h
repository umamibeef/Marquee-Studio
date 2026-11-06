/* lwIP architecture glue: PowerPC MPC862, big-endian, freestanding. */
#ifndef ARCH_CC_H
#define ARCH_CC_H

#define BYTE_ORDER              BIG_ENDIAN
#define LWIP_NO_INTTYPES_H      1
#define LWIP_NO_CTYPE_H         1
#define LWIP_NO_UNISTD_H        1

#define X8_F    "02x"
#define U16_F   "hu"
#define S16_F   "hd"
#define X16_F   "hx"
#define U32_F   "lu"
#define S32_F   "ld"
#define X32_F   "lx"
#define SZT_F   "zu"

/* No console: diagnostics vanish, a failed assertion stops here so the watchdog resets. */
#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { for (;;) ; } while (0)

#endif
