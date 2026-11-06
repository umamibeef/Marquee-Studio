# How the reverse engineering is verified

Every claim about the firmware should be checkable by a machine as well as by reading the decompile. `./check.sh`
(repo root) runs every check; `./check.sh --full` adds the slow co-simulations. The levels, from weakest to strongest:

## 1. The recreated source compiles (`analysis/tools/compile_check.py`)

The named decompile (`analysis/ghidra_out/elite_rgb_named.c`) and the generated header (data types, one prototype
per function, every global with the decompiler's type) are compiled with clang for 32-bit big-endian PowerPC.
- Catches wrong signatures (argument counts, a `void` result that is used), wrong struct fields, broken types.
- Does not catch a wrong name or meaning: a function can compile perfectly and still be misunderstood.
- Runs at the end of every `apply_symbols.sh`. It fails if a function that compiled before stops compiling,
  and otherwise raises the baseline. A sentinel error at the end of the file proves clang read all of it.
- Assembly is counted apart. Functions whose `symbols.tsv` comment carries the tag `[asm]` were hand-written
  assembly in the original: the exception vectors (0x100-0x1F00), the alignment-exception emulator, the context
  switch, `isr_entry_dispatch`, `setjmp`/`longjmp`, `__eabi`, and `nrv2_decompress` with its bit reader, which
  returns in CR0 and r3. A rebuild supplies them as assembly. The report line "C functions (not tagged [asm])" is
  the real target. Since round 15 every C function compiles (1858/1858), and 1914/1919 overall.
- Source rewrites (`sanitize`), on top of the unsigned compare below: string labels that keep `<`, `>`, `.`,
  `-` (`s_..._ADDR`) are spelt as the header declares them; Ghidra's array casts (`local = (char [4])x._0_4_;`,
  `arr == (byte [4])0`) become word accesses; a call through a word global with no function type
  (`(*_DAT_0045c5b4)()`, RTEMS configuration hooks) is cast to `code *`. A `fn` type in `symbols.tsv`
  (`fn*`) types callback globals (`display_hook_a..d`).
- `tools/argregs.py` finds how many register arguments a function really takes, from the machine code: which
  of r3..r10 are read before they are written on some path, through callees and tail calls (`--mismatch` lists
  every prototype that disagrees). It settles argument-count errors. Ghidra drops an argument at a call site when the
  caller passes one of its own parameters straight through and the callee's prototype is not locked
  (`blit_bg_solid`'s colour in r9, `rtems_interrupt_catch`'s `old` in r5): an explicit signature in `symbols.tsv`
  locks it. A double argument after one int sits in the aligned pair r5:r6 (EABI), so r4 is a dummy parameter in
  `_d2b`, `_dtoa_r` and `vfprintf_cvt`.

## 2. Claims about named functions (`analysis/tools/test_claims.py`)

Short assertions that run the firmware's own machine code in Unicorn (through `tools/fw.py`) and check what its
name and comment promise. For example, `toupper('a') == 'A'`, `display_model_name(0x80C0) == "AD0201"`, and
`sbuf_pad_left` right-aligns.
- Catches wrong names and wrong comments. On its first run it found that `sbuf_set_hex`'s third argument is the
  exact number of digits rather than a minimum.
- Rule: when you name a function whose behaviour can be checked in isolation, add a claim.
- `tools/fw.py` can call any function by name with ints, C strings, `sbuf`s or raw buffers. It stubs RTOS calls,
  and `fw.stub(name, value)` stubs hardware the emulator does not model (for example clock registers).

The larger component tests of the same kind: `test_message_markup.py` (the message tokenizer), `test_crcs.py` (all
CRCs, JWAM byte-stuffing, `tools/signproto.py`), `test_mt.py` (`/dev/random` = MT19937), `tools/test_nvram_decode.py`
and `tools/test_gif2flc.py`.

## 3. The recreated source is equivalent to the binary (`analysis/tools/difftest.py`)

This is the strongest check on the source itself. For a function, its decompiled C is compiled back to PowerPC and linked
with every function and global it uses bound to the real firmware address. The original and the recompiled code
then run in two identical emulators on the same random inputs. Pointer arguments point into a pool of buffers whose
words are a mix of valid pointers, small ints and printable text. The return value and every byte of RAM are
compared.
- `equivalent`: matched on every trial that ran. `different`: both completed with different results or memory, so
  the recreated C does not behave like the firmware. `fault_differs`: one side faulted and the other did not.
  That usually means random inputs aliasing the code's own structures (a buffer pointing into its header), so it is
  reported but weaker. `untested`: every trial faulted on both sides, or the inputs made the code overwrite
  itself, so there is no information.
- Not counted as differences: words that hold a stack address on both sides (the two builds lay out their stacks
  differently), and trials whose writes hit the app's code area.
- `--selftest` breaks a few functions on purpose (one operator or constant each) and requires them to be reported
  `different`. This proves the checker can fail.
- `difftest_list.txt` lists functions that must stay equivalent. `./check.sh` runs it. `difftest_full.txt` holds
  every other function the last full sweep found equivalent (656 more); `./check.sh --full` runs it.
- `--all` sweeps every app function that compiles; the results are in `ghidra_out/check/difftest.txt`.
  Sweeps of 2026-10-31: first run 711 equivalent / 11 different; second (every app function named) 728 / 9;
  third (with the width, signed-char and stack-local rules below) 735 equivalent, 2 different (the two that
  cannot be, see below), 17 fault_differs, 304 untested, 64 do not build alone. No function lost equivalence.
- Access widths. Ghidra often types globals as 1-byte `undefined`. `ExportDecomp.java` writes the real access
  width of every fixed address per function (`ghidra_out/elite_rgb_named.access.tsv`, from the p-code), and
  difftest declares each such global with that width. When one function reads an address at two widths (or the
  read goes through a base register and is not in the table), each use's own statement decides
  (`*(undefined2 *)p = g;`, or a 2-byte local). Pointer globals are declared with the width of what they
  point to, taken from the casts they are assigned through anywhere in the program (`spi_init` assigns
  `_DAT_0045c3e8 = (undefined2 *)...`, `spi_done` only indexes it), because Ghidra's `g + 4` / `g[1]` scale by it.
- No strict aliasing, wrapping ints (`-fno-strict-aliasing -fwrapv`, round 15). The decompiled C reaches the
  same bytes through differently typed pointers, and the firmware's code assumes neither rule. `font_build(f, l,
  &f->img)` copies an image onto itself shifted by a word: with type-based alias analysis clang reordered the copy
  and `tools/rebuild.py` saw the difference from the image.
- `[asm]` functions are reported as `asm` and not compared.
- Edge trials (round 15): before the random draws, one trial per int argument sets that argument to 0 and
  every other int argument to a full 32-bit value. Before this, `crc16_kermit_update(crc, buf, 0)` returning the
  CRC unmasked was found only by `tools/rebuild.py`.
- Firmware division by zero: a trial in which the firmware's own `divw`/`divwu` (found by scanning the
  function) divides by zero is skipped. That is undefined behaviour in C, and clang used it to drop
  `effect_texture_8001`'s later `h != 0` test.
- Sweep of round 15: 761 equivalent, 4 different (none fixable in C, see ANALYSIS.md), 16 fault_differs, 288
  untested, 63 nobuild, 28 asm.
- `char` is signed, as in Ghidra (`-fsigned-char`; clang's PowerPC default is unsigned). FLI LC run counts are
  `(char)` casts.
- Stack locals that Ghidra splits apart are retyped with `stack` rows in `symbols.tsv` (ApplySymbols): the firmware
  indexes past a local array into the variable after it (`effect_texture_8002`'s stripe colours) or a callee fills
  a struct that Ghidra cut in two (`odometer_init_first`'s value record). Recompiled, the pieces are laid out
  independently; as one array they behave like the firmware.
- Two functions cannot be equivalent: `nrv2_getbit` (an assembly helper returning in two registers, swapped relative to
  the C convention) and `eeprom_section_checksum` called with an invalid section (the firmware then loops over
  whatever r9/r10 hold). Both behave as the firmware on every meaningful input.
- Most early "differences" were faults in the checker itself. These were fixed, and the self-test still catches every
  broken case:
  - `_DAT_x` aliases were declared with the 1-byte type of `DAT_x`. Ghidra's `_` prefix means a different access
    size, so they are now words.
  - Random arguments made the firmware overwrite its own code.
- A typical finding: Ghidra's pseudo-C keeps signedness implicit. For example, `sbuf_to_int`'s digit test
  `(byte)c - 0x30 < 10` is unsigned in the machine code (`cmplwi`) but signed in C, so the recompiled C accepts
  characters below '0'. In such places the source reads correctly yet would not build the same firmware. `compile_check.py`
  now rewrites this pattern (`byte - k < n`) into an explicit unsigned compare, so the compilable source matches
  the binary. The difftest confirmed it on both functions where it appeared. The second sweep found the same
  pattern through a cast pointer (`*(byte *)(p + i) - 0x30 < 10`, `decimal_field_parse`); that form is rewritten
  too.

## 4. Whole-system checks

- The bootloader rebuilt in C is co-simulated against the original boot ROM (`make cosim`): boot to the app
  entry, byte-identical RAM, every board scenario. Also `make netsim` and `make emojitest`.
- Hardware tests stay manual and read-only (see the safety rules in BACKLOG.md / bootloader/README.md).

## What is still only "probable"

Names ending in `_maybe` (`probable` in symbols.tsv) have no claim or test yet. Turning them into claims is how
they become `known`.

## First boot on the real sign (2026-11-02): what the simulator missed

The from-source image passed every simulator check, then reset-looped on the sign. Run from RAM through our boot
loader's window (no chip writes) made each fix testable on the board; a hardware bisection (601 recompiled
functions, the board's own watchdog resets driving each step) found the rest. All five are real-hardware effects:

1. Hardware registers in the recompiled C were not `volatile`: `cpm_command` read the CPCR busy flag once and spun
   on the copy. Fix: `difftest.exact_widths` declares every global in a device range volatile.
2. Ghidra dropped the write-back that clears write-1-to-clear event registers (`SCCE = SCCE` looks like a no-op):
   `scc3_isr` and `digi_codec_isr` never left the interrupt. Fix: `rebuild.py` keeps the original instructions of any
   function whose C lost a hardware-register store (`tools/device_store_check.py`); `hw_exclude.tsv` records them.
3. A DMA buffer from `malloc` is outside SDRAM bank 0: the MMU maps the banks back to back virtually but 32 MB apart
   physically, and the CPM uses the address as physical. The console sent zeros. Fix: `ext_dma_scratch` (fwext.py),
   in the extension block in bank 0; the crash report's buffer too.
4. The data cache must be written back (`dcbst`) before a DMA reads a buffer.
5. The simulator did not model the CPU's software watchdog. It does now; `flashtest` fails on any expiry.

Also seen: after a cold power-on the board resets once, 7 to 19 s in (cause unknown). Tools that use the serial
window let it happen first (boot once, catch the next window).
