# minipro: the TL866A voltage patch

The boot flash (AM29LV040B, 3.3 V) is read and programmed with minipro. Upstream minipro (0.7.4, master 3e2f491)
drives 3.3 V chips at **5 V on a TL866A/CS**. With the chip fitted, VCC measured up to 4.1 V, above the
AM29LV040B's 4.0 V absolute maximum. **Never use stock minipro with a TL866A on 3.3 V chips.** The TL866II+/T48
drivers pass the database voltages through and are not affected.

## The bug

The TL866A database packs the VPP code (high nibble) and the supply VCC code (low nibble) into the low byte of
`voltages`. The start-transaction message carries that byte in byte 5 and the high byte in byte 8. minipro's
`tl866a.c` sent `vpp << 4`, which moves the VCC nibble into the VPP nibble and leaves VCC at 0 (5 V). The shift
came in with commit 802b0d65 (2022-09-11). Before that, byte 5 was sent unshifted, as the vendor software does.
Upstream issues #387 (this chip and programmer) and #375 (`--vcc` has no effect on the supply) report the problem.

## The patch

`tl866a-voltages.patch` applies to upstream 3e2f491. The same change is a commit on
https://gitlab.com/umamibeef/minipro, branch `tl866a-voltages`, prepared for a merge request upstream. For the
TL866A/CS:
- byte 5 carries the database low byte unchanged (#387);
- `--vpp` / `-o vpp=` change only the VPP nibble (before, they also overwrote the supply VCC);
- `--vcc` / `-o vcc=` also set the supply nibble (before, they changed only the verify code in byte 8, #375);
- `-d` and the write summary decode VPP and the supply VCC from the right nibbles, and add a "VCC supply" line.

For all programmers, the patch fixes the bit-bang VCC lookup, which used the VPP table.

It was tested without hardware, using a harness that loads every TL866A database entry and captures the message.
For all 4,895 non-bit-bang devices, byte 5 and byte 8 equal the database low and high byte, and `--vpp` / `--vcc`
set the intended nibbles.

## Build

```sh
git clone https://gitlab.com/DavidGriffith/minipro && cd minipro
git checkout 3e2f491 && git apply ../tools/minipro/tl866a-voltages.patch
make && make install PREFIX=<somewhere>
```

`tools/dump_bootflash.sh` and `tools/tl866_voltcheck.py` expect the patched build (set `MINIPRO=` to its path).

## Before programming

At the 3.3 V setting the TL866A measured **about 3.7 V on the empty adapter**, above the AM29LV040B's 3.6 V
operating maximum. Reading still worked. Prefer a T48 or TL866II+ for writing. With the TL866A, fit the spare chip
and measure VCC first. See `docs/FLASHING.md`.

Seating: chips or adapters with fewer than 40 pins go **towards the ZIF handle**, with pin 1 next to the handle
(TL866 manual).
