# The LED panel FPGA (ELITE_RGB V4.20)

The LED panel is driven by an FPGA that sits at 0xF0000000 and receives each frame by IDMA (see ANALYSIS.md,
"LED panel output path"). On some boards the firmware has to load that FPGA's configuration at start-up. This page
covers when that happens, how, and which design is used. The whole sequence is checked by
`analysis/tools/test_fpga.py`, which runs the real `fpga_configure` against a fake FPGA in Unicorn (part of `./check.sh`).

## Where the bitstreams are

There are four bitstreams in the application image, `fpga_bitstream_a`..`d` at 0xA216C, 0xA56B0, 0xA8BF4 and
0xAC138 (0x3544 bytes each, 0x3541 sent). The application is the NRV2D-compressed `ELITE_RGB` entry of the 512 KB
boot flash (at 0x26D4 in `dumps/bootflash.bin`), so the bitstreams are on that chip, inside the compressed app.

- Format: Xilinx serial configuration (Spartan / XC4000 style). Preamble `FF`, then `2` and a 24-bit length
  count of 109,025 bits (0x01A9E1); 0x3541 bytes = 109,064 bits are sent.
- It is TWO XCS05XL images behind one header (checked 2026-11-02 against DS060 Table 17: XCS05XL = 127 bits per
  frame, 429 frames, 54,491 bits of program data). Frames of 127 bits, each with a 0 start bit, run at phase 40 from
  bit 40 to about 54,523 (429 frames), then at phase 45 from bit 54,528 for another 429 frames to about 109,011.
  There is no second header; the one length count covers both. In a Xilinx daisy chain the first device keeps its
  frames and passes the rest out of DOUT to the next device.
  - Image 1 is the same in both designs (a/c and b/d). Image 2 holds every A/E-series difference (first differing
    bit 59,811).
  - Image 1 is presumably U6's. Where U6's DOUT goes, and so which FPGA takes image 2, is not known yet.
- The FPGA on this sign's board (U6, photographed 2026-11-02) is a Xilinx Spartan **XCS05XL**, VQ100 package,
  speed grade -4, commercial (marking `XCS05XL VQ100AKP0341 D1282457A 4C`, date code 0341).
  - There is no configuration PROM next to it: U17, the 8-pin chip beside U6, is marked about "1626 / 25042" (no
    Xilinx logo; perhaps a small 25-series SPI EEPROM, not identified). So the firmware configures U6, as for board
    type 2 (table below). The board's silkscreen says "...eries 2." (Series 2).
  - Our own FPGA design would be a new XCS05XL image (or pair of images) sent the same way.
- The panel path (photos 2026-11-02, local only): CPU (IDMA) -> CPU-board FPGA U6 -> five National DS34C87 quad
  RS-422 line drivers (U8, U9, U18, U21, U23: 20 differential pairs) -> the "LED Panel Interface" ribbon (a latched
  header, about 50-way, not counted) -> the Row Processor's five MAX3094E quad receivers -> its FPGA and dual-port
  RAM -> LED board 1 (RGB-02) -> LED boards 2..5, daisy-chained.
- The daughter board is the "Advanced Row Processor", Part No. RP-02 Rev 2.10, "Copyright (c), 2003 NEURIZON Pty
  Ltd" (label "MIK341-010-45"). There is one for the whole sign. It plugs into the first LED board and drives all five: the LED
  boards are daisy-chained, so the whole 200 x 16 frame goes out through it. "40X16" in its name is presumably the
  LED board size it is made for. It has its own FPGA, which loads from its own PROM:
  - U8: Xilinx XCS05XL VQ100 -4C (date 0337). U9: Xilinx serial PROM marked "17S05LVC" (XC17S05L family), sized
    for the XCS05XL. (So image 2 is probably not for U8, unless the PROM is unused or the board can take either.)
  - U7: Cypress CY7C025AV, 8K x 18 3.3 V dual-port SRAM, probably the frame buffer between the input and the scan.
  - U2..U6: five MAX3094E quad RS-485/RS-422 receivers (20 differential inputs). U10, U11: 74HC244 buffers.
  - U12: 16.000 MHz oscillator. U13: Sipex SPX29150T regulator. U14: hex rotary switch labelled "ROW NO.".
  - Its "ROW NO." hex switch is set to 0. What it selects is not confirmed (this sign has one 16-px row; the
    firmware's models go up to 5 rows).
- The LED boards: "RGB 40x16 LED Array Panel, Part No. RGB-02 Rev 3.1", label "MIKRGB40X16 05040568", ECN 007,
  (c) Neurizon Pty Ltd 2003.
- Other parts seen on the CPU board: U43 ADM241L (RS-232, the DB9), U39 ADM489 (RS-485), U36..U38 PC410L optocouplers (isolated
  RS-485, presumably), P17 "Remote Control 1" (the IR receiver).
- Working assumption (user's decision, not traced on the board): the firmware configures both FPGAs. Image 1 goes
  to U6 and image 2 down the daisy chain (U6 DOUT, pin 73) over the ribbon to the Row Processor's U8; U8's PROM U9
  is then unused or a fall-back. DS060 confirms the chain format: the header and length count pass down the chain
  and each FPGA captures them, so only the first image has a header. XCS05XL VQ100 configuration pins: DIN P72,
  DOUT P73, CCLK P74, DONE P50, PROGRAM P52.
- There are only two designs. a == c and b == d byte for byte. The two designs differ in 606 bytes, all past
  offset 0x1D34.

## Which design: the A/E series bit

`ledpanel_init` calls `fpga_configure(eeprom_display_bit14())`. The argument is bit 14 of the display geometry
word in the config EEPROM (+0x40), the A/E series flag of the display model (AD0201..AD0505 vs
ED0203IM..ED0505; see ANALYSIS.md, "Display models").

| argument (E series) | EEPROM options bit 1 = 0 | options bit 1 = 1 |
|---|---|---|
| 1: E-series display | fpga_bitstream_a | fpga_bitstream_c (= a) |
| 0: A-series display | fpga_bitstream_b | fpga_bitstream_d (= b) |

This sign (config EEPROM read 2026-11-02): word 0xC240 has bit 14 set and the options word is 0, so it loads
`fpga_bitstream_a`, the E-series design.

So in V4.20 the design depends only on the display series. Options bit 1 selects between identical copies,
perhaps a leftover from a version where they differed.

## When the firmware loads it: board type and revision

Config EEPROM byte +4 is the board type, and +5 is the revision:

| board | panel lines | FPGA loaded by the firmware |
|---|---|---|
| type 1, revision < 0x50 | Port D (old layout) | no |
| type 1, revision >= 0x50 | Port C/B | no (`board_fpga_preloaded`: revision >= 5; presumably it loads itself from a PROM) |
| type 2 | Port C/B | yes |

`ledpanel_init` also picks the panel refresh timing from EEPROM +8, the CPU clock in Hz. It uses 350 Hz on the
old layout at 40 MHz, otherwise 280 Hz.

## The load sequence (`fpga_configure`, 0x5A764)

1. PA4 (PROG) is a GPIO output, idle high. PB17 is an input (INIT) and PB19 an output, idle high. Wait 100 us.
2. Take the SPI (`spi_lock`) and set master mode, 8-bit, MSB first, at 1 MHz (`spi_set_speed(0x670, 1000000)`).
3. Pulse PROG low for 300 us. This clears the FPGA.
4. If DONE is already high (PIPR bit 0x02000000), go back to step 3.
5. Wait until INIT (PB17) is high, then drive PB19 low, wait 100 us.
6. Send the bitstream with `spi_transfer(tx = bitstream, rx = 0x43FF9C, 0x3541)`, about 0.11 s at 1 MHz, and wait
   for `spi_done`.
7. Drive PB19 high again.
8. Done when DONE is high and every byte received during the download is 0xFF (the data-in line stayed high).
   Otherwise go back to step 3. The test shows two bad readbacks give three downloads.

The pin names come from how the firmware uses them, matched to the Xilinx serial-configuration signals. PROG, INIT
and DONE are standard; what PB19 is wired to (the chip's CS or a buffer enable) is an inference.

## Changing the panel logic

A new bitstream would ship like the colour-emoji patch: data the boot loader places in RAM over the app's copy.
Writing one needs either the original design files or a new design for the same part. That is a large job and is
not started.
