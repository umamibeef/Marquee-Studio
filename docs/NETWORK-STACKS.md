# Network stack options for the CPU-002 board (2026-10-30)

## Platform facts (from the firmware and board)
- CPU: MPC862P. It has a built-in Fast Ethernet Controller (FEC) with an MII interface.
- PHY: Intel LXT972A (10/100, MII), 25 MHz crystal. Expected ID registers: reg 2 = `0x0013`,
  reg 3 = `0x78Ex`. If the MDDIS pin (3) is strapped high, MDIO is disabled and the PHY runs from its strap pins.
- Firmware: RTEMS 4.5/4.6 era, built around 2003, going by libpng 1.2.5 (Oct 2002), zlib 1.1.4 and IMFS linear
  files. It is statically linked, has no symbols, and no source is available. No network code is linked in:
  SCC1 is named `scc1_unused`, and the code never touches the FEC registers.
- Pins: the firmware reconfigures Port D, where the MPC862 multiplexes the FEC MII pins.
  - A self-test routine saves Port D, toggles pins, then restores it.
  - It drives PD9 (0x40) as a GPIO output (decompile ~line 55445) and reads PD8 (0x80) (~line 61916).
  - To verify: check PD8/PD9 against the MPC862 Port D pin-function table before relying on the FEC.
- Resources: about 128 MB SDRAM, so RAM is no constraint. The app is NRV2D-packed in the 512 KB socketed boot
  flash and runs from RAM at address 0. About 85 KB of the boot flash is free.

## Candidate stacks
| Stack | MPC8xx FEC driver | Footprint | Fit |
|---|---|---|---|
| RTEMS libnetworking (legacy FreeBSD) | Yes: the RTEMS MPC8xx "RTEMS/TCPIP" driver, SCC1 or FEC (`tqm8xx/network/network_fec.c`). Originally 10 Mbps only; fixed later. | Large; mbuf pools default to 1 MB each | Era-matched to the firmware, but it must be linked into the RTEMS executive, so it needs a firmware rebuild. |
| rtems-libbsd (modern FreeBSD) | Yes: `if_ffec_mpc8xx.c`, ported in 2018, used on the TQM8xx BSP | Heaviest | Needs RTEMS 5/6, so only for a full re-platform (new firmware). The best-maintained driver. |
| lwIP, NO_SYS raw API | No lwIP FEC driver exists; port one from the RTEMS or Linux `fec_8xx` driver | About 15 to 22 KB of code (2003 figures) | Injectable. Runs polled, with no RTEMS integration. Has UDP, TCP and DHCP, and is maintained. |
| uIP | Same: port a driver | About 5 KB of code, one packet buffer | The easiest to inject. Enough for a UDP or simple TCP command port. Upstream is inactive. |
| External bridge (ESP32 or Raspberry Pi → RS-485) | Not applicable | Not applicable | No firmware risk. Reuses `tools/signproto.py`. |

## Integration paths
1. Inject a polled stack into the existing firmware. This is the recommended route for Ethernet on the sign.
   - Add lwIP NO_SYS (UDP first) or uIP, plus a minimal FEC driver (buffer-descriptor rings, MII/MDIO setup), to
     ELITE_RGB.
   - Call it from an existing periodic loop.
   - Hand received UDP payloads to the existing RS-485 message handler `FUN_0002f544`. It takes 69-byte records
     at `0x12dd0c`, so network packets become ordinary sign messages.
   - Re-pack with an NRV2D compressor (UCL/UPX) and program a spare AM29LV040B. The original chip stays
     untouched, so this is fully reversible.
   - No RTEMS internals are touched, because the driver is polled and needs no interrupt hookup.
2. Rebuild the firmware on RTEMS 4.11, 5 or 6 with libnetworking or libbsd. Everything would need re-implementing
   from the decompilation, which is months of work.
3. External bridge. Ready in days, with no firmware changes.

## Recommendation
- For Ethernet on the sign itself: lwIP NO_SYS raw API, UDP only, static IP (add DHCP later).
  - Port the FEC driver from RTEMS `network_fec.c` (the same chip family), injected as in path 1.
  - lwIP is preferred over uIP for TCP and DHCP headroom and an active upstream. RAM and flash allow it.
  - uIP is the fallback if flash space runs short.
- If the goal is only network control, use the external bridge.

## Checks before committing
1. Prove the MII wiring. With the sign on and Port D idle, bit-bang MDC/MDIO from a Pico.
   Read PHY reg 2/3 and expect `0x0013` / `0x78Ex`. Also check the MDDIS pin level.
2. Rule out pin conflicts. Compare PD8/PD9 GPIO use with the MPC862 FEC pin mux.
   If they overlap, find out what the board wires them to.
3. Find a safe hook point. Locate a periodic task to call the stack's poll function from.
   Confirm the patched, re-packed image fits in the boot flash.

Sources: RTEMS-libbsd `if_ffec_mpc8xx.c` (git.rtems.org); RTEMS commits "bsp/tqm8xx: Fix network interface driver"
(2018-03) and "if_ffec_mpc8xx: Import legacy driver from RTEMS" / "Port driver to libbsd" (2018-09); RTEMS
`rtems_bsdnet_config` docs; Dunkels, uIP/lwIP, MobiSys 2003; lwip-users RTEMS port announcement (2016-08).

## Implemented (2026-10-31): network window in the rebuilt boot loader
`bootloader/` with `make NET=1` adds lwIP 2.2.1 and a polled MPC862 FEC driver to the C rebuild
of BOOTSTRAP V2.00. During boot it serves:
- DHCP, or the static fallback 169.254.77.1;
- ping;
- TFTP: get the boot flash, main flash or NVRAM, or put an app image to boot it from RAM.

It is verified only in emulation (`make netsim`). Details and the hardware caveats (Port D wiring
by board variant, PHY address) are in `bootloader/README.md`. The same driver and lwIP port can
later be moved into the app (path 1 above), so Ethernet also works while the sign runs.
