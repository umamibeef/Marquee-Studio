/* CPM I2C master for the configuration EEPROMs (0xA0 board config, 0xA2 UPM table / SPD).
 * Translated from 0xF8182050 (init), 0xF8182138 (stop), 0xF8182144 (read),
 * 0xF81821F8 (page write, unused by the bootstrap) and 0xF81822CC (checksum).
 * Buffer descriptors live at DPRAM 0x408 (RX), 0x410 and 0x418 (TX). */
#include "board.h"

#define BD16(a)  (*(volatile uint16_t *)(a))
#define BD32(a)  (*(volatile uint32_t *)(a))
#define TXB(n)   (*(volatile uint8_t *)(I2C_TXBUF + (n)))

void i2c_init(void)
{
	PBODR |= 0x30;                      /* PB26/PB27 open drain: I2CSDA/I2CSCL */
	PBDIR |= 0x30;
	PBPAR |= 0x30;
	I2MOD = 0;

	*(volatile uint16_t *)(I2C_PRAM + 4) = 0x1010;   /* RFCR/TFCR */
	*(volatile uint16_t *)(I2C_PRAM + 6) = 0x0100;   /* MRBLR */
	I2ADD = 0x80;
	I2BRG = 0x10;
	I2CER = 0x17;
	I2CMR = 0;
	I2MOD = 1;

	*(volatile uint16_t *)(I2C_PRAM + 0) = 0x2408;   /* RBASE */
	*(volatile uint16_t *)(I2C_PRAM + 2) = 0x2410;   /* TBASE */

	BD32(I2C_RXBD + 4) = 0;
	BD32(I2C_RXBD + 0) = 0x20000000;                 /* W, len 0 */
	BD32(I2C_TXBD0 + 4) = I2C_TXBUF;
	BD32(I2C_TXBD0 + 0) = 0x04000002;
	BD32(I2C_TXBD1 + 4) = 0;
	BD32(I2C_TXBD1 + 0) = 0x2C000000;

	CPCR = 0x0011;                                   /* INIT RX AND TX, I2C channel */
	while (CPCR & 0x0001)
		;
}

void i2c_stop(void)
{
	I2MOD = 0;
}

/* Random read of len bytes from offset 0 of device dev.
 * Returns 0 when done, 1 if the receive BD is still empty, 200 on overrun. */
int i2c_read(uint8_t dev, uintptr_t buf, uint32_t len)
{
	uint16_t st;

	BD32(I2C_RXBD + 4) = buf;
	BD32(I2C_RXBD + 0) = 0xA0000000;                 /* E | W */
	TXB(0) = dev & 0xFE;                             /* address, write */
	TXB(1) = 0;                                      /* offset 0 */
	TXB(2) = (dev & 0xFE) | 1;                       /* repeated start, read */
	BD32(I2C_TXBD1 + 4) = I2C_TXBUF + 2;
	if (len == 0)
		return 0;

	BD16(I2C_TXBD1 + 2) = (uint16_t)(len + 1);
	BD16(I2C_TXBD1 + 0) = 0xAC00;                    /* R | W | L | S */
	BD32(I2C_TXBD0 + 0) = 0x84000002;                /* R, 2 bytes */
	I2COM = 0x81;                                    /* master, start */
	delay_ms(200);

	st = BD16(I2C_RXBD);
	if (st & 0x8000)
		return 1;
	if (st & 0x0002)
		return 200;
	return 0;
}

/* Page write in 8-byte chunks. Present in the original, never called. */
int i2c_write(uint8_t dev, uintptr_t buf, int len)
{
	uint32_t off = 0;
	uint16_t st;

	TXB(0) = dev & 0xFE;
	while (len > 0) {
		int n = len < 8 ? len : 8;

		TXB(1) = (uint8_t)off;
		BD32(I2C_TXBD1 + 4) = buf + off;
		BD16(I2C_TXBD1 + 2) = (uint16_t)n;
		BD16(I2C_TXBD1 + 0) = 0xA800;
		BD32(I2C_TXBD0 + 0) = 0x84000002;
		I2COM = 0x81;
		delay_ms(30);
		if (BD16(I2C_TXBD0) & 0x8000)
			return 1;

		BD32(I2C_TXBD0 + 0) = 0xA4000001;        /* address only: ack poll */
		I2COM = 0x81;
		delay_ms(2);
		st = BD16(I2C_TXBD0);
		if (st & 0x8000)
			return 1;
		if (st & 0x0007)
			return 201;

		off += 8;
		len -= n;
	}
	return 0;
}

/* Byte sum (low 8 bits) and byte xor over buf[0..len). */
void eeprom_sum(uintptr_t buf, uint32_t len, uint8_t *sum, uint8_t *xr)
{
	uint32_t s = 0, x = 0, i;

	for (i = 0; i < len; i++) {
		uint8_t c = *(volatile uint8_t *)(buf + i);
		s += c;
		x ^= c;
	}
	*sum = (uint8_t)s;
	*xr = (uint8_t)x;
}
