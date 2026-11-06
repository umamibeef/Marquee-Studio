#!/usr/bin/env python3
"""Executable claims: each named function does what its name and symbols.tsv comment say.

Every claim runs the firmware's own code (tools/fw.py, Unicorn) and checks the result. When a name is wrong, its
claim fails. Add a claim whenever you name a function whose behaviour can be checked in isolation.
Usage: test_claims.py [-v] [name-substring]
"""
import bz2, random, struct, sys, zlib
import fw
from fw import FW, SYM
from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_5, UC_PPC_REG_PC, UC_PPC_REG_LR

CLAIMS = []


def claim(fn):
    CLAIMS.append(fn)
    return fn


# ---- C library ----
@claim
def libc_strings(f):
    a = f.cstr("abc", 32)
    f.call("strcat", a, f.cstr("def"))
    assert f.read_cstr(a) == "abcdef"
    assert f.call("strlen", f.cstr("hello")) == 5
    assert f.call_s("strcmp", f.cstr("abc"), f.cstr("abd")) < 0
    assert f.call("strcmp", f.cstr("same"), f.cstr("same")) == 0
    assert f.call("strncmp", f.cstr("abcX"), f.cstr("abcY"), 3) == 0
    d = f.alloc(16)
    f.call("strcpy", d, f.cstr("copy"))
    assert f.read_cstr(d) == "copy"


@claim
def libc_ctype_abs(f):
    assert f.call("toupper", ord("a")) == ord("A") and f.call("toupper", ord("Z")) == ord("Z")
    assert f.call("tolower", ord("Q")) == ord("q") and f.call("tolower", ord("1")) == ord("1")
    assert f.call_s("abs", -42) == 42 and f.call("abs", 7) == 7


@claim
def libc_memory(f):
    a = f.alloc(16)
    f.call("memset", a, 0x5A, 10)
    assert f.read(a, 11) == b"\x5a" * 10 + b"\0"
    b = f.alloc(16)
    f.call("memcpy", b, f.bytes_(b"0123456789"), 10)
    assert f.read(b, 10) == b"0123456789"


# ---- sbuf ----
@claim
def sbuf_basics(f):
    sb = f.sbuf("abc", cap=8)
    assert f.call("sbuf_len", sb) == 3 and f.call("sbuf_cap", sb) == 8
    f.call("sbuf_putc", sb, ord("d"))
    assert f.sbuf_text(sb) == "abcd"
    f.call("sbuf_set_char", sb, 0, ord("X"))
    assert f.sbuf_text(sb) == "Xbcd"
    assert f.call("sbuf_remove_at", sb, 1) == ord("b") and f.sbuf_text(sb) == "Xcd"
    f.call("sbuf_insert_char", sb, 1, ord("Y"))
    assert f.sbuf_text(sb) == "XYcd"
    assert f.read_cstr(f.call("sbuf_cstr", sb)) == "XYcd"


@claim
def sbuf_numbers(f):
    assert f.call_s("sbuf_to_int", f.sbuf("-123x")) == -123
    assert f.call("sbuf_to_int", f.sbuf("456")) == 456
    sb = f.sbuf("42", cap=8)
    f.call("sbuf_pad_left", sb, 5, ord("0"))
    assert f.sbuf_text(sb) == "00042", f.sbuf_text(sb)
    sb = f.sbuf("", cap=8)
    f.call("sbuf_set_hex", sb, 0xBEEF, 4)
    assert f.sbuf_text(sb).upper() == "BEEF", f.sbuf_text(sb)
    f.call("sbuf_set_hex", sb, 0xBEEF, 2)                    # exactly `digits` digits: the low ones
    assert f.sbuf_text(sb).upper() == "EF", f.sbuf_text(sb)


@claim
def sbuf_compare(f):
    assert f.call("sbuf_equal_nocase", f.sbuf("Hello"), f.sbuf("hELLO")) == 1
    assert f.call("sbuf_equal_nocase", f.sbuf("Hello"), f.sbuf("Hell")) == 0
    assert f.call("sbuf_equal_nocase_cstr", f.sbuf("true"), f.cstr("TRUE")) == 1


# ---- small helpers ----
@claim
def bcd_decoders(f):
    for n in ("bcd_to_u32_mkhn", "bcd_to_u32_trbo", "bcd_to_u32_mkcl"):
        assert f.call(n, f.bytes_(b"\x12\x34\x56"), 3) == 123456, n
        assert f.call(n, f.bytes_(b"\x1a"), 1) == 0xFFFFFFFF, n + " (invalid BCD)"


@claim
def bcd_wide_and_reversed(f):
    r = f.call("bcd_to_u64_gass", f.bytes_(b"\x12\x34\x56\x78\x90\x12"), 6, r4=True)
    assert (r[0] << 32 | r[1]) == 123456789012, r
    r = f.call("bcd_to_u64_mkhn", f.bytes_(b"\x99\x99"), 2, r4=True)
    assert (r[0] << 32 | r[1]) == 9999, r
    assert f.call("bcd_to_u32_reversed", f.bytes_(b"\x56\x34\x12"), 3) == 123456


@claim
def sound_pan(f):
    ch = f.alloc(0x74)
    for pan, want in ((0x80, (200, 200)), (0x40, (100, 200)), (0xC0, (200, 98))):
        f.call("sound_set_pan", ch, 200, 200, pan)
        got = (f.u32(ch + 0x14), f.u32(ch + 0x18))
        assert got == want, (pan, got)


@claim
def shift_right_pads(f):
    for name, n, extra in (("shift_right_pad4_bar", 4, ()), ("shift_right_pad10_readout", 10, ()),
                           ("shift_right_pad11_number", 11, ()), ("shift_right_pad_n", 6, (5,))):
        s = f.bytes_(b"ABCDEFGHIJK"[:n])
        f.call(name, s, *extra)
        assert f.read(s, n) == (b" " + b"ABCDEFGHIJK"[:n - 1]), (name, f.read(s, n))


@claim
def controller_message_unpack(f):
    REC = 0x11F4
    base = f.alloc(REC * 3, 16)
    pk = lambda w: struct.pack(">I", w)
    stream = (b"\x0f" + b"\x1c" + pk(0x7C000000) + b"HI" + b"\x1b" + pk(0x01000F0E) + b"\x1c" + pk(0xFFFFFFFF)
              + b"X" + b"\x1b" + pk(0))
    cur = f.alloc(4)
    f.u32(cur, f.bytes_(stream))
    assert f.call("text_line_db_unpack", 1, base, len(stream) + 1, cur) == 1
    rec = base + REC
    n = f.u16(rec + 10)
    got = [(c, f.u32(rec + 0x10C + 4 * i)) for i, c in enumerate(f.read(rec + 0xC, n))]
    assert got == [(0x48, 0x7C000000), (0x49, 0x7C000000), (0, 0x01000F0E), (0x1C, 0x7C000000),
                   (0x58, 0x7C000000), (0x1B, 0x7C000000)], got
    assert f.read(rec + 8, 1)[0] == 0x0F


@claim
def geometry_and_ipow(f):
    for word, w, h in ((0x80C0, 80, 16), (0xC244, 200, 80), (0x8140, 120, 16)):
        assert f.call("geometry_width_px", word) == w and f.call("geometry_height_px", word) == h, hex(word)
    assert f.call("ipow", 10, 3) == 1000 and f.call("ipow", 7, 1) == 7 and f.call("ipow", 0, 5) == 0
    assert f.call("sbuf_equal", f.sbuf("Abc"), f.sbuf("Abc")) == 1
    assert f.call("sbuf_equal", f.sbuf("Abc"), f.sbuf("abc")) == 0


@claim
def g711_decoders(f):
    import audioop
    s16 = lambda r: r - 0x10000 if r & 0x8000 else r
    for name, ref in (("alaw_to_linear", audioop.alaw2lin), ("ulaw_to_linear", audioop.ulaw2lin)):
        got = [s16(f.call(name, b) & 0xFFFF) for b in range(256)]
        want = [struct.unpack("<h", ref(bytes([b]), 2))[0] for b in range(256)]
        assert got == want, name


@claim
def wav_sample_readers(f):
    import audioop
    def sound(data):
        snd = f.alloc(0xC0)
        buf = f.bytes_(data)
        f.u32(snd + 0xAC, buf)
        f.u32(snd + 0xB0, buf)
        f.u32(snd + 0xB4, len(data))
        return snd
    l, r = f.alloc(2), f.alloc(2)
    s16 = lambda v: v - 0x10000 if v & 0x8000 else v
    snd = sound(struct.pack("<hh", 1234, -2))                 # 16-bit little-endian, stereo
    assert f.call("wav_next_pcm16_stereo", snd, l, r) == 1
    assert (s16(f.u16(l)), s16(f.u16(r))) == (1234, -2)
    snd = sound(b"\xd5")                                       # A-law, mono: same sample on both sides
    assert f.call("wav_next_alaw_mono", snd, l, r) == 1
    want = struct.unpack("<h", audioop.alaw2lin(b"\xd5", 2))[0]
    assert s16(f.u16(l)) == want and s16(f.u16(r)) == want
    assert f.call("wav_next_alaw_mono", snd, l, r) == 0         # end of data


@claim
def path_extension(f):
    p = f.cstr("dir/name.flc")
    assert f.read_cstr(f.call("path_extension", p)) == "flc"
    p = f.cstr("noext")
    assert f.call("path_extension", p) == p + 5


@claim
def str_match_step(f):
    pat, pos = f.cstr("OK"), f.alloc(4)
    seen = [f.call("str_match_step", pat, ord(c), pos) for c in "xxOK"]
    assert seen == [0, 0, 0, 1], seen


@claim
def display_model_names(f):
    out = f.alloc(16)
    for word, name in ((0x80C0, "AD0201"), (0x90C2, "AD0203IM"), (0xC244, "ED0505")):
        f.call("display_model_name", word, out)
        assert f.read_cstr(out) == name, (hex(word), f.read_cstr(out))
    assert f.call("display_type_from_config", 0x80C0) == 0
    assert f.call("display_type_from_config", 0x1234) == 0x1A
    assert f.call("display_config_from_type", 25) == 0xC244


@claim
def cpm_brg(f):
    clk = 50_000_000                                          # the clock registers are not emulated
    f.stub("brg_clock_hz", clk)
    v = f.call("cpm_brg_value", 9600)
    div16 = v & 1
    cd = (v >> 1) & 0xFFF
    got = clk / ((cd + 1) * 16 * (16 if div16 else 1))
    assert v & 0x10000 and abs(got - 9600) / 9600 < 0.02, (hex(v), clk, got)


@claim
def menu_stack(f):
    depth0 = f.u32(SYM["menu_stack_depth"])
    cur0 = f.u32(SYM["menu_current"])
    f.call("menu_stack_push", 0x12345678)
    assert f.u32(SYM["menu_current"]) == 0x12345678
    assert f.call("menu_stack_pop") == cur0 and f.u32(SYM["menu_stack_depth"]) == depth0


@claim
def menu_item_accessors(f):
    item = SYM["menu_items"] + 2 * 0x58                       # "Display Number:" (after menu_handler set-up)
    f.call("menu_handler", 1, 0xFFFF, limit=20_000_000)
    assert f.read_cstr(f.call("menu_item_label", item, 160)) == "Display Number:"
    assert f.call("menu_item_max", item) == 999 and f.call("menu_item_min", item) == 0
    f.call("menu_item_set", 123, item)
    assert f.call("menu_item_get", item) == 123
    assert f.call("menu_item_can_view", item, 1) == 1 and f.call("menu_item_can_view", item, 0) == 0


# ---- RTEMS CPU port ----
@claim
def rtems_interrupt_catch_and_vector_install(f):
    table = f.alloc(0x48 * 4)
    f.u32(0x45C498, table)
    old = f.alloc(4)
    f.u32(table + 5 * 4, 0x1111)
    assert f.call("rtems_interrupt_catch", 0x2222, 5, old) == 0
    assert f.u32(old) == 0x1111 and f.u32(table + 5 * 4) == 0x2222
    assert f.call("rtems_interrupt_catch", 0x2222, 0x48, old) == 10     # RTEMS_INVALID_NUMBER
    assert f.call("rtems_interrupt_catch", 0, 5, old) == 9               # RTEMS_INVALID_ADDRESS
    assert f.call("rtems_interrupt_catch", 0x2222, 5, 0) == 9


@claim
def cpu_context_initialize_sets_stack_and_entry(f):
    ctx = f.alloc(0x80)
    f.call("cpu_context_initialize", ctx, 0x100000, 0x2000, 0, 0xABCD00)
    assert f.u32(ctx) == 0x100000 + 0x2000 - 8 and f.u32(0x100000 + 0x2000 - 8) == 0
    assert f.u32(ctx + 0x16 * 4) == 0xABCD00
    assert f.u32(ctx + 0x17 * 4) & 0x8000                                # level 0: interrupts (MSR EE) on
    f.call("cpu_context_initialize", ctx, 0x100000, 0x2000, 1, 0xABCD00)
    assert not f.u32(ctx + 0x17 * 4) & 0x8000


@claim
def modem_rx_reset_empties_ring(f):
    for a in (0xB8680, 0xB8684, 0xB8698):
        f.u32(a, 7)
    f.call("modem_rx_reset")
    assert [f.u32(a) for a in (0xB8680, 0xB8684, 0xB8698)] == [0, 0, 0]
    assert f.call("sys_getpid_r") == 0 and f.call("sys_kill_r", 0, 0, 9) == 0


@claim
def sbuf_pop_count_and_paths(f):
    sb = f.sbuf("banana")
    assert f.call("sbuf_count_char", sb, ord("a")) == 3
    assert f.call("sbuf_pop_char", sb) == ord("a") and f.sbuf_text(sb) == "banan"
    assert f.call("sbuf_pop_char", f.sbuf("")) == 0
    p = f.cstr("C:/dir/file.txt")
    assert f.read_cstr(f.call("path_basename", p)) == "file.txt"
    assert f.read_cstr(f.call("path_basename", f.cstr("noslash"))) == "noslash"
    d = f.cstr("/a/b", 16)
    f.call("path_add_slash", d)
    assert f.read_cstr(d) == "/a/b/"
    f.call("path_add_slash", d)
    assert f.read_cstr(d) == "/a/b/"


@claim
def jackpot_relay_event_timer(f):
    from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_PC, UC_PPC_REG_LR
    calls = []

    def on_relay(uc, addr, size, _):
        calls.append((uc.reg_read(UC_PPC_REG_3), uc.reg_read(UC_PPC_REG_4)))
        uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))
    f.uc.hook_add(1 << 2, on_relay, begin=SYM["relay_set"], end=SYM["relay_set"])
    f.u32(SYM["cfg_jackpot_relay_timeout"], 2)
    for mode, want in ((0, [(0, 0), (1, 0)]), (1, [(0, 1), (1, 0)]), (2, [(0, 0), (1, 1)])):
        f.u32(0xE2D88, mode)                             # relay mode: bit 0 = relay 0, bit 1 = relay 1
        calls.clear()
        f.call("jackpot_relay_event", 1)
        assert calls == want, (mode, calls)
        f.call("jackpot_relay_event", 2)                 # reset
    f.u32(0xE2D88, 3)
    calls.clear()
    f.call("jackpot_relay_event", 1)
    assert calls == [(0, 1), (1, 1)], calls
    calls.clear()
    for _ in range(2 * 40 - 1):
        f.call("jackpot_relay_event", 0)
    assert calls == []                                   # still inside cfg_jackpot_relay_timeout x 40 frames
    f.call("jackpot_relay_event", 0)
    assert calls == [(0, 0), (1, 0)], calls              # released on the 80th frame


@claim
def cpu_context_switch_saves_and_loads(f):
    import fw as fwmod
    from unicorn.ppc_const import UC_PPC_REG_1, UC_PPC_REG_13, UC_PPC_REG_31, UC_PPC_REG_MSR
    run, heir = f.alloc(0x60), f.alloc(0x60)
    msr = f.uc.reg_read(UC_PPC_REG_MSR)
    words = [0x00F00000, 0x2222] + [0x1300 + i for i in range(19)]       # r1, r2, r13..r31
    f.write(heir, struct.pack(">21I", *words) + struct.pack(">III", 0x24000000, fwmod.STOP, msr))
    f.call("cpu_context_switch", run, heir)                              # "returns" through the heir's LR
    assert f.uc.reg_read(UC_PPC_REG_1) == 0x00F00000
    assert [f.uc.reg_read(UC_PPC_REG_13 + i) for i in range(19)] == words[2:]
    assert f.uc.reg_read(UC_PPC_REG_31) == 0x1312
    assert f.u32(run) == fwmod.STACK and f.u32(run + 0x58) == fwmod.STOP  # the caller's sp and LR were saved


@claim
def image_scale_blit_nearest_scales(f):
    def image(w, h, pix=None):
        p = f.alloc(w * h * 2)
        if pix:
            f.write(p, struct.pack(f">{w * h}H", *pix))
        img = f.alloc(20)
        f.write(img, struct.pack(">IIIII", 1, w, h, p, 0))
        return img, p
    src, _ = image(2, 2, [1, 2, 3, 4])
    dst, dp = image(8, 8)
    rect, pos, rows, cols = f.alloc(16), f.alloc(8), f.alloc(4), f.alloc(4)
    f.write(rect, struct.pack(">iiii", 0, 0, 2, 2))
    f.write(pos, struct.pack(">ii", 1, 1))
    f.call("image_scale_blit_nearest", src, dst, rect, pos, 2000, 2000, rows, cols)
    out = struct.unpack(">64H", bytes(f.read(dp, 128)))
    grid = [out[y * 8 + 1:y * 8 + 5] for y in range(1, 5)]
    assert grid == [(1, 1, 2, 2), (1, 1, 2, 2), (3, 3, 4, 4), (3, 3, 4, 4)], grid
    assert (f.u32(rows), f.u32(cols)) == (4, 4)


# ---- probable -> known (round 12) ----
@claim
def sbuf_number_formatting(f):
    sb = f.sbuf("x")
    f.call("sbuf_append_uint", sb, 1234)
    assert f.sbuf_text(sb) == "x1234", f.sbuf_text(sb)
    f.call("sbuf_append_uint", sb, 0)
    assert f.sbuf_text(sb) == "x12340"                                   # 0 appends a single "0"
    f.call("sbuf_set_number", sb, 4294967295)
    assert f.sbuf_text(sb) == "4294967295"                               # clears first; unsigned 32-bit


@claim
def display_and_board_bits(f):
    for word, bit in ((0x80C0, 0), (0xC141, 1)):
        f.u16(SYM["display_config_word"], word)
        assert f.call("display_config_bit14") == bit
    for typ, rev, want in ((1, 5, 1), (1, 0x50, 1), (1, 4, 0), (2, 9, 0)):
        f.write(SYM["eeprom_board_type"], bytes([typ, rev]))
        assert f.call("board_fpga_preloaded") == want, (typ, rev)
    f.stub("board_is_spd", 0)
    base = SYM["eeprom_board_type"] - 4 + 0x14                           # config EEPROM copy +0x14..+0x19
    f.write(base, bytes(6))
    assert f.call("eeprom_id_bytes_present") == 0
    f.write(base + 5, b"\x01")
    assert f.call("eeprom_id_bytes_present") == 1


@claim
def png_read_cb_mem_reads_in_order(f):
    data = f.bytes_(b"ABCDEFGH")
    st = f.alloc(12)
    f.write(st, struct.pack(">III", data, 0, 8))                         # {base, position, size}
    f.stub("png_get_io_ptr", st)
    out = f.alloc(8)
    f.call("png_read_cb_mem", 0, out, 5)
    assert bytes(f.read(out, 5)) == b"ABCDE" and f.u32(st + 4) == 5
    f.write(out, b"zzzz")
    f.call("png_read_cb_mem", 0, out, 4)                                 # past the end: nothing copied
    assert bytes(f.read(out, 4)) == b"zzzz" and f.u32(st + 4) == 5


@claim
def sbuf_append_cstr_stops_at_capacity(f):
    sb = f.sbuf("ab", cap=6)
    f.call("sbuf_append_cstr", sb, f.cstr("cdefgh"))
    assert f.sbuf_text(sb) == "abcdef"                                   # stops at the capacity
    f.call("sbuf_append_cstr", sb, f.cstr("x"))
    assert f.sbuf_text(sb) == "abcdef"


@claim
def rect_clip_clips_or_rejects(f):
    b, r = f.alloc(16), f.alloc(16)
    f.write(b, struct.pack(">iiii", 0, 0, 80, 16))
    f.write(r, struct.pack(">iiii", -5, 10, 20, 10))
    assert f.call("rect_clip", b, r) == 1
    assert struct.unpack(">iiii", bytes(f.read(r, 16))) == (0, 10, 15, 6)
    f.write(r, struct.pack(">iiii", 100, 0, 5, 5))
    assert f.call("rect_clip", b, r) == 0
    f.write(r, struct.pack(">iiii", 80, 0, 5, 5))                        # touching counts as overlapping
    assert f.call("rect_clip", b, r) == 1


@claim
def tlux_escape_stx_and_soh(f):
    sb = f.sbuf("", cap=32)
    f.call("sbuf_set_bytes", sb, f.bytes_(b"\x02A\x02\x01B"), 5)
    f.call("tlux_escape", sb)
    got = bytes(f.read(f.u32(sb), f.u16(sb + 6)))
    assert got == b"\x02A\x02\x00\x02\xffB", got.hex()                  # 02 -> 02 00, 01 -> 02 FF (not the first byte)


@claim
def decimal_field_parse_first_digit_run(f):
    v, w = f.alloc(4), f.alloc(4)
    for text, want in ((b"  1234 5", 1234), (b"AB0042CD", 42), (b"--------", 0), (b"/:12345678"[:8], 123456)):
        f.call("decimal_field_parse", f.bytes_(text), v, w)
        assert f.u32(v) == want and f.u32(w) == want, (text, f.u32(v), f.u32(w))   # both outputs get the value


@claim
def theme_ini_value_finds_value(f):
    text = f.bytes_(b"Key=Value\r\nNext=1")
    out = f.alloc(4)
    f.call("theme_ini_value", text, 0, 18, out)
    assert f.u32(out) == text + 4
    f.call("theme_ini_value", text, 11, 18, out)
    assert f.u32(out) == text + 16
    f.call("theme_ini_value", f.bytes_(b"NoEquals\r\nX=1"), 0, 14, out)
    assert f.u32(out) == 0                                               # stops at the end of the line


@claim
def protocol_reply_finish_crc_and_framing(f):
    frame = bytes([0x01, 0x02, 0x03, 0x00, 0, 0, 0x7E, 0x55, 0x7D, 0xAA])  # bytes 4/5 get the length
    f.write(0x179E20, frame)
    f.u32(0x45C35C, len(frame))
    f.u32(0x45C354, 2)                                                   # reply slot 2 (0x100 bytes each)
    f.call("wsll_reply_finish")
    body = bytearray(frame)
    body[4], body[5] = (len(frame) - 6) & 0xFF, (len(frame) - 6) >> 8
    crc = 0xFFFF                                                         # reflected CCITT 0x8408, init 0xFFFF, no xor-out
    for b in body:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    body += bytes([crc & 0xFF, crc >> 8])
    want = b"\x7e" + b"".join(bytes([0x7D, b ^ 0x20]) if b in (0x7D, 0x7E) else bytes([b]) for b in body) + b"\x7e"
    got = bytes(f.read(0x179620 + 2 * 0x100, len(want)))
    assert got == want, (got.hex(), want.hex())
    assert f.u32(0x45C358) == len(want)


@claim
def dma_region_splits_at_bank_boundary(f):
    f.u32(0x14, 0x1000000)                                               # top of RAM
    f.u32(0x54, 0x400000)                                                # bank size
    for n, base in enumerate([0x10000000, 0x20000000, 0xFFFFFFFF, 0x30000000]):
        f.u32(0x58 + 4 * n, base)                                        # bank bases (-1: not present)
    out, ln = f.alloc(4), f.alloc(4)
    f.call("dma_region", 0x3FFF00, 0x200, out, ln)
    assert (f.u32(out), f.u32(ln)) == (0x103FFF00, 0x100)                # cut at the bank end
    f.call("dma_region", 0x400010, 0x20, out, ln)
    assert (f.u32(out), f.u32(ln)) == (0x20000010, 0x20)
    f.call("dma_region", 0x800000, 0x10, out, ln)
    assert (f.u32(out), f.u32(ln)) == (0, 0)                             # bank not present
    f.call("dma_region", 0xFFFFF0, 0x20, out, ln)
    assert (f.u32(out), f.u32(ln)) == (0, 0)                             # runs past the top of RAM


@claim
def amount_format_modes(f):
    def run(value, decimals, mode, dec=b".", thou=b",", prefix="$", suffix=""):
        cfg = f.alloc(0x50)
        f.write(cfg + 0x3A, bytes([decimals]))
        f.write(cfg + 0x48, dec + thou)
        f.u32(cfg + 0x4C, mode)
        out = f.sbuf("", cap=64)
        f.call("amount_format", out, value, f.sbuf(prefix), f.sbuf(suffix), cfg)
        return bytes(f.read(f.u32(out), f.u16(out + 6)))
    big = lambda s: bytes(c - 0x20 if 0x30 <= c <= 0x39 else c for c in s)     # odometer digits 0x10..0x19
    assert run(123456789, 2, 1) == b"$1,234,567.89"                             # mode 1: plain; value in minor units
    assert run(5, 2, 1) == b"$0.05" and run(0, 0, 1) == b"$0"
    assert run(123456789, 2, 0) == b"\x1c" + big(b"1,234,567.89")              # mode 0: all odometer digits, $ -> 0x1C
    assert run(123456789, 2, 2) == b"\x1c" + big(b"1,234,567.") + b"\x08\x09"  # mode 2: + small cents 0x00..0x09
    assert run(123456789, 2, 3) == b"$1,234,567.\x08\x09"                      # mode 3: plain + small cents
    assert run(123456, 2, 1, b",", b".", "EUR ", " pts") == b"EUR 1.234,56 pts"
    assert run(1234567, 2, 1, thou=b"\0") == b"$12345.67"                       # separator 0: none


# ---- probable -> known (round 13) ----
@claim
def dpram_block_reserve_release(f):
    ptrs = [0xBD8FC + 4 * i for i in range(4)]                           # bsp_early_alloc bump pointers
    for i, a in enumerate(ptrs):
        f.u32(a, 0xFF002000 + 0x200 * i)
    for a in (0xBD934, 0xBD938, 0xBD93C, 0xBD940):
        f.u32(a, 0)
    assert f.call("dpram_block_reserve", 1) == 0xFF002200
    assert f.call("dpram_block_reserve", 1) == 0                      # already reserved
    assert f.call("dpram_block_reserve", 3) == 0xFF002600
    f.call("dpram_block_release", 1)
    assert f.call("dpram_block_reserve", 1) == 0xFF002200             # back after the release
    f.u32(ptrs[0], 0xFF002010)                                           # 16 bytes already handed out
    assert f.call("dpram_block_reserve", 0) == 0                      # a used block is not reserved


@claim
def theme_get_value_group_priority(f):
    from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_PC, UC_PPC_REG_LR
    seen = []

    def hook(uc, addr, size, _):
        g = uc.reg_read(UC_PPC_REG_3)
        seen.append(g)
        uc.reg_write(UC_PPC_REG_3, 1 if g == want else 0)
        uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))
    a = SYM["theme_parse_group_global"]
    f.uc.hook_add(1 << 2, hook, begin=a, end=a)
    for want, order in ((4, [4]), (2, [4, 3, 2]), (0, [4, 3, 2, 1, 0]), (9, [4, 3, 2, 1, 0])):
        seen.clear()
        r = f.call("theme_get_value", f.cstr("Key"), f.sbuf(""))
        assert seen == order and r == (want != 9), (want, seen, r)


@claim
def level_lookups(f):
    f.write(0xDCDF8, bytes([7, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 9] + [0] * 15))  # 8-byte entries
    assert f.call_s("level_index_from_address", 3) == 1
    assert f.call_s("level_index_from_address", 5) == -1
    f.write(SYM["cfg_level_address"], bytes([1, 2, 1, 3, 2, 2, 0, 0]))   # (group, level) pairs
    assert f.call("mkcl_level_find", 1, 3) == 2                    # index + 1
    assert f.call("mkcl_level_find", 2, 1) == 0


@claim
def lfs_entry_nth_walks_the_chain(f):
    vol = bytearray()
    for name, data in (("a.png", b"AAAA"), ("bb.flc", b"BBBBBBBB"), ("c.wav", b"C")):
        nb = name.encode()
        head = struct.pack(">III", 0, len(data), 0) + bytes([len(nb)]) + nb
        head = head.ljust(0xC + ((len(nb) + 4) & ~3), b"\0")
        e = head + data + b"\0" * 4
        e += b"\0" * (-len(e) % 4)
        vol += struct.pack(">I", len(e)) + e[4:]
    vol += bytes(16)
    base = f.bytes_(bytes(vol))
    h = f.alloc(0x40)
    for n, name, data in ((1, "bb.flc", b"BBBBBBBB"), (0, "a.png", b"AAAA"), (2, "c.wav", b"C")):
        assert f.call("lfs_entry_nth", h, base, len(vol), n) == 1     # the handle's valid flag (+0x10)
        assert f.read_cstr(f.u32(h + 0x24)).startswith(name)             # +0x24: name
        assert bytes(f.read(f.u32(h), len(data))) == data                # +0: data
    assert f.call("lfs_entry_nth", h, base, len(vol), 3) == 0      # past the last entry


@claim
def sound_slot_ids(f):
    f.u32(0xB87A4, 1)                                                    # winner sounds loaded
    f.u32(0xB8798, 1)                                                    # attract sounds loaded
    f.u32(0x181BD0 + 2 * 0x13C, 0x1234)                                  # winner02.wav present
    f.u32(0x1792CC + 15 * 0x13C, 0x5678)                                 # id 15 = attract04.wav present
    f.u32(0x45C364, 0)                                                   # logo_s.wav absent
    assert f.call("sound_slot_loaded", 2) == 1 and f.call("sound_slot_loaded", 3) == 0
    assert f.call("sound_slot_loaded", 15) == 1 and f.call("sound_slot_loaded", 16) == 0
    assert f.call("sound_slot_loaded", 7) == 0                     # 5..10: no sounds
    assert f.call("sound_slot_loaded", 200) == 0
    f.u32(0x45C364, 1)
    assert f.call("sound_slot_loaded", 200) == 1
    f.u32(0xB87A4, 0)
    assert f.call("sound_slot_loaded", 2) == 0                     # bank not loaded: nothing is


# ---- probable -> known (round 15) ----
@claim
def mpc8xx_clock_tree(f):
    """SCCR (IMMR+0x280) and PLPRCR (+0x284) fields, as in the MPC860/862 manuals."""
    sccr, plprcr = 0xFF000280, 0xFF000284
    f.u32(0x1C, 10_000_000)                                              # PLL input clock (hand-off block)
    f.u32(0x18, 32768)                                                   # the other reference clock
    f.u32(plprcr, 4 << 20)                                               # MF = 4: VCO = 5 x 10 MHz
    f.u32(sccr, 0)
    assert f.call("pll_out_hz") == 50_000_000
    for n in ("cpu_clock_hz", "sys_clock_hz", "bus_clock_hz", "sync_clock_hz", "brg_clock_hz"):
        assert f.call(n) == 50_000_000, n
    f.u32(sccr, 2 << 5)                                                  # DFNH = 2
    assert f.call("cpu_clock_hz") == 12_500_000
    f.u32(plprcr, (4 << 20) | 0x400)                                     # CSRC: DFNL path, / 2^(DFNL+1)
    f.u32(sccr, 1 << 8)
    assert f.call("cpu_clock_hz") == 12_500_000
    f.u32(plprcr, 4 << 20)
    f.u32(sccr, 1 << 11)                                                 # DFBRG = 1: / 4
    assert f.call("brg_clock_hz") == 12_500_000 and f.call("cpu_clock_hz") == 50_000_000
    f.u32(sccr, 1 << 13)                                                 # DFSYNC = 1: / 4
    assert f.call("sync_clock_hz") == 12_500_000
    f.u32(sccr, 1 << 17)                                                 # EBDF = 1: CLKOUT = GCLK / 2
    assert f.call("bus_clock_hz") == 25_000_000 and f.call("cpu_clock_hz") == 50_000_000
    for v, want in ((0, 32768 // 4), (0x800000, 10_000_000 // 4), (0x1000000, 32768 // 512)):
        f.u32(sccr, v)                                                   # RTSEL picks the clock, RTDIV /4 or /512
        assert f.call("pitrtc_clock_hz") == want, (hex(v), f.call("pitrtc_clock_hz"))


@claim
def pit_tick_init(f):
    f.u32(0x18, 4_000_000)                                               # PITRTCLK = 1 MHz
    f.u32(0xFF000280, 0)
    f.u32(0xBDDFC, 10000)                                                # 10 ms per tick
    f.u32(0xBDE00, 1)
    caught = []

    def hook(uc, addr, size, _):
        caught.append((uc.reg_read(UC_PPC_REG_3), uc.reg_read(UC_PPC_REG_4)))
        uc.reg_write(UC_PPC_REG_3, 0)
        uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))
    a = SYM["rtems_interrupt_catch"]
    f.uc.hook_add(1 << 2, hook, begin=a, end=a)
    f.call("system_tick_init", 0x1234, 3)
    assert caught == [(0x1234, 0x19 + 2 * 3)]                            # SIU level 3 vector
    assert f.u16(0xFF000244) == 10000                                    # PITC
    assert f.u16(0xFF000240) == (0x8000 >> 3) | 0x85                     # PISCR: PIRQ level 3, PS, PIE, PTE
    assert f.u32(0xFF000014) == 0x01000000                               # SIMASK LVL3


@claim
def cpm_timer_programming(f):
    f.u32(0x1C, 10_000_000)
    f.u32(0xFF000284, 4 << 20)                                           # GCLK 50 MHz
    f.u16(0xFF000980, 0xFFFF)
    f.call("cpm_timer_set_period_ns", 3, 280)                            # the LED panel clock: 14 GCLKs
    assert f.u16(0xFF0009A0) == 0x2A and f.u16(0xFF0009A4) == 6           # TMR3 OM|FRR|ICLK=GCLK, TRR3 = 7 - 1
    assert f.u16(0xFF000980) == 0x70FF and f.u16(0xFF0009B4) == 0xFFFF   # TGCR timer 3 bits cleared, TER3
    f.call("cpm_timer_set_period_ns", 1, 10_000_000)                     # 10 ms: prescaler 3, (62499+1)*4*2 GCLKs
    assert f.u16(0xFF000990) == 0x32A and f.u16(0xFF000994) == 62499
    f.call("cpm_timer_set_tin_count", 2, 1000)
    assert f.u16(0xFF000992) == 0x0E and f.u16(0xFF000996) == 999         # FRR | ICLK=TIN
    f.call("cpm_timer_set_tin_count", 4, 0x74 * 0x300)
    assert f.u16(0xFF0009A2) == 0x10E and f.u16(0xFF0009A6) == (0x74 * 0x300 - 1) // 2


@claim
def random_range_bounds(f):
    f.call("mt_sgenrand", 4357)
    rd = SYM["random_read"]

    def hook(uc, addr, size, _):                                         # rtems_io_read -> the /dev/random driver read
        uc.reg_write(UC_PPC_REG_PC, rd)
    a = SYM["rtems_io_read"]
    f.uc.hook_add(1 << 2, hook, begin=a, end=a)
    vals = {f.call_s("random_range", 5, 9) for _ in range(200)}
    assert vals == {5, 6, 7, 8, 9}, vals


@claim
def machine_pattern_match_modes(f):
    cases = (("ABC123", "ABC"), ("ABC123", "123"), ("ABC123", "C12"), ("ABC123", "X"))
    want = {0: [1, 0, 0, 0], 1: [0, 1, 0, 0], 2: [1, 0, 0, 0], 3: [1, 1, 1, 0]}
    for mode, w in want.items():
        got = [f.call("machine_pattern_match", f.sbuf(t), mode, f.cstr(p)) for t, p in cases]
        assert got == w, (mode, got)
    assert f.call("machine_pattern_match", f.sbuf("ABC"), 3, f.cstr("")) == 0


@claim
def pool_record_copy_quirk(f):
    for i in range(-1, 3):
        f.write(0xB426C + i * 0xFC, bytes([0x20 + i]) * 0xFC)
    d = f.alloc(0x100)
    for pool, want in ((0, 0x20), (1, 0x20), (2, 0x20), (-1, 0x1F)):
        f.call("pool_record_copy", d, pool)
        assert f.read(d, 0xFC) == bytes([want]) * 0xFC, pool


@claim
def nvram_block_store_snapshot(f):
    import zlib
    base = f.alloc(0x1400)
    f.u32(0x24, base)                                                    # NVRAM base, size (hand-off block)
    f.u32(0x28, 0x1000)
    p = f.call("nvram_block_store", f.cstr("GLOBAL_PARAMS"), f.bytes_(b"DEFAULTS"), 8)
    h = f.read(base, 1)[0]                                               # 1 + 13 + 3 pad + size + crc
    assert h == 25 and p == base + h and f.read(p, 8) == b"DEFAULTS"
    assert f.read(base + 1, 16) == b"GLOBAL_PARAMS\0\0\0" and f.u32(base + 17) == 8
    assert f.u32(base + 21) == zlib.crc32(f.read(base, 21))              # header CRC only
    f.write(p, b"CHANGED!")
    assert f.call("nvram_block_store", f.cstr("GLOBAL_PARAMS"), f.bytes_(b"NEWDATA!"), 8) == p
    assert f.read(p, 8) == b"NEWDATA!"                                   # found: the data is written again
    heap = f.alloc(0x1000)
    f.stub("malloc", heap)
    f.u32(0xBDB80, 0)
    f.call("nvram_snapshot_take")                                        # copy of NVRAM minus the top 0x410 bytes
    assert f.u32(0xBDB80) == heap and f.read(heap, 0x40) == f.read(base, 0x40)
    f.write(heap + p - base, b"SNAPSHOT")
    f.call("nvram_snapshot_take")                                        # only the first time
    assert f.read(heap + p - base, 8) == b"SNAPSHOT"
    assert f.call("nvram_snapshot_find", f.cstr("GLOBAL_PARAMS"), 8) == heap + h
    assert f.call("nvram_snapshot_find", f.cstr("GLOBAL_PARAMS"), 9) == 0    # size must match too
    assert f.call("nvram_snapshot_find", f.cstr("OTHER"), 8) == 0


@claim
def nvram_records_and_jackpot_take(f):
    import zlib
    buf = f.alloc(0x2B8 * 3)
    f.u32(0xBB178, buf)
    f.call("nvram_record_set_u16", 1, 0xBEEF)
    r = f.read(buf + 0x2B8, 0x2B8)
    assert r[:2] == b"\xbe\xef" and struct.unpack(">I", r[0x2B4:])[0] == zlib.crc32(r[:0x2B4])
    f.write(0x450A4C + 2 * 0x14, struct.pack(">5I", 1, 2, 3, 4, 5))
    out = f.alloc(20)
    f.call("jackpot_value_take", 2, out)
    assert struct.unpack(">5I", f.read(out, 20)) == (1, 2, 3, 4, 5)
    assert struct.unpack(">5I", f.read(0x450A4C + 0x28, 20)) == (1, 0, 3, 4, 5)   # the "new" word cleared


@claim
def text_line_db_clear_and_model_params(f):
    import zlib
    db = f.alloc(0x2000)
    f.write(db, b"\x12" * 0x2000)
    f.stub("nvram_block_get", db)
    f.call("text_line_db_clear")
    assert f.u16(db) == 0xFFFF                                           # empty chain
    assert f.u32(db + 0x1FFC) == zlib.crc32(f.read(db, 0x1FFC))
    blk = f.alloc(12)
    f.stub("nvram_block_get", blk)
    f.stub("version_string_check", 0)
    f.call("model_params_store", f.bytes_(b"ABCDEFGHIJKL"))
    assert f.read(blk, 12) == b"ABCDEFGHIJKL" and f.u32(SYM["cfg_group_nvram"] + 8) == blk
    out = f.alloc(12)
    f.call("model_params_fetch", out)
    assert f.read(out, 12) == b"ABCDEFGHIJKL"


@claim
def rng_pick_maps_into_ranges(f):
    rg = f.alloc(0x810)                                                  # lo[] at +0xC, hi[] at +0x408, count +0x804
    for i, (lo, hi) in enumerate(((10, 12), (20, 25), (100, 100))):
        f.u32(rg + 0xC + 4 * i, lo)
        f.u32(rg + 0x408 + 4 * i, hi)
    f.u32(rg + 0x804, 3)
    st = f.alloc(0x700)
    f.u32(0xDD3BC, 0)                                                    # same number not allowed: keep a history
    f.u32(0xDDFB8, 0)
    for k, want, rng in ((0, 10, (10, 12)), (2, 12, (10, 12)), (3, 20, (20, 25)), (8, 25, (20, 25)),
                         (9, 100, (100, 100)), (1, 11, (10, 12))):
        f.stub("random_range", k)                                        # the k-th number of the union of ranges
        assert f.call("rng_pick_from_ranges", st, rg, 0, 9) == 1
        assert f.u32(st + 0x63C) == want and (f.u32(st + 0x634), f.u32(st + 0x638)) == rng, k
    hist = [f.u32(0xDDBBC + 4 * i) for i in range(f.u32(0xDDFB8))]
    assert hist == [10, 11, 12, 20, 25, 100]                             # drawn numbers, kept sorted
    f.stub("random_range", 10)                                           # past the end: nothing drawn
    assert f.call("rng_pick_from_ranges", st, rg, 0, 9) == 0 and f.u32(st + 0x63C) == 0
    assert (f.u32(st + 0x634), f.u32(st + 0x638)) == (10, 100)           # overall min / max
    f.u32(0xDD3BC, 1)
    f.stub("random_range", 4)
    assert f.call("rng_pick_from_ranges", st, rg, 0, 9) == 1 and f.u32(0xDDFB8) == 6   # no history kept


@claim
def ac97_master_attenuation(f):
    seen = []

    def hook(uc, addr, size, _):
        seen.append((uc.reg_read(UC_PPC_REG_3), bytes(uc.mem_read(uc.reg_read(UC_PPC_REG_5), 8))))
        uc.reg_write(UC_PPC_REG_3, 0)
        uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))
    a = SYM["rtems_io_control"]
    f.uc.hook_add(1 << 2, hook, begin=a, end=a)
    f.u32(0x3C4E58, 7)                                                   # /dev/digi major
    for v in (0xF8, 0, 0x48):
        f.call("ac97_master_attenuation_set", v)
    msg = lambda reg, v: struct.pack(">IBxH", 3, reg, v)                 # {u32 3, u8 register, pad, u16 value}
    assert [s[1] for s in seen] == [msg(2, 0x1F1F), msg(6, 0x1F), msg(2, 0x0909), msg(6, 0x09)]


def nrv2d_encode(data):
    """A small UCL NRV2D encoder (8-bit bit buffer, greedy matches in a 4 KB window) for the claims below."""
    out, st = bytearray(), {"pos": 0, "n": 8}

    def bit(b):
        if st["n"] == 8:
            st["pos"], st["n"] = len(out), 0
            out.append(0)
        out[st["pos"]] |= (b & 1) << (7 - st["n"])
        st["n"] += 1

    def gamma_off(t):                     # decoder: m = 1; loop { m = 2m + a; if stop: break; m = 2(m - 1) + c }
        seq = []
        while True:
            a, p = t & 1, t >> 1
            seq.append(("a", a))
            if p == 1:
                break
            seq.append(("c", p & 1))
            t = (p >> 1) + 1
        seq.reverse()
        for k, (kind, v) in enumerate(seq):
            bit(v)
            if kind == "a":
                bit(1 if k == len(seq) - 1 else 0)

    def gamma_len(m):                     # decoder: m = 1; do m = 2m + b; while (!stop)
        bs = bin(m)[3:]
        for i, b in enumerate(bs):
            bit(int(b))
            bit(1 if i == len(bs) - 1 else 0)
    i = 0
    while i < len(data):
        best_len, best_d = 0, 0
        for d in range(1, min(i, 4096) + 1):
            n = 0
            while i + n < len(data) and data[i + n - d] == data[i + n] and n < 300:
                n += 1
            if n > best_len:
                best_len, best_d = n, d
        L, d = best_len, best_d
        if L >= 3 + (d > 0x500):
            bit(0)
            n = L - 1 - (d > 0x500)
            x = ((d - 1) << 1) | (1 - (n >> 1 if n <= 3 else 0))
            gamma_off((x >> 8) + 3)
            out.append(x & 0xFF)
            if n <= 3:
                bit(n & 1)
            else:
                bit(0)
                gamma_len(n - 2)
            i += L
        else:
            bit(1)
            out.append(data[i])
            i += 1
    bit(0)                                # end marker: m_off = 0xFFFFFFFF
    gamma_off(0x1000002)
    out.append(0xFF)
    return bytes(out)


@claim
def nrv2d_decompress_in_place(f):
    import random
    rnd = random.Random(1)
    data = (b"ABCABCABCABC hello hello hello " * 5 + bytes(rnd.randrange(4) for _ in range(3000)) + b"Z" * 400
            + bytes(range(256)) * 3)
    enc = nrv2d_encode(data)
    dst = f.alloc(len(data) + 16)
    src = f.bytes_(enc)                                                  # the output must stay below the input
    assert f.call("nrv2_decompress", 0, len(enc), src, dst, limit=50_000_000) == len(data)
    assert f.read(dst, len(data)) == data
    f2 = FW()
    src2 = f2.bytes_(enc)
    dst2 = f2.alloc(len(data) + 16)                                      # output above the input: refused
    assert f2.call("nrv2_decompress", 0, len(enc), src2, dst2) == 0
    f3 = FW()
    dst3 = f3.alloc(len(data) + 16)
    src3 = f3.bytes_(enc)
    assert f3.call("nrv2_decompress", 0, len(enc) - 10, src3, dst3, limit=50_000_000) == 0   # input too short


# ==== library pass (round 15): RTEMS ====
@claim
def rtems_heap_allocate_free_extend(f):
    heap, area = f.alloc(0x1C), f.alloc(0x2000)
    assert f.call("_Heap_Initialize", heap, area, 0x1000, 3) == 0          # page size not a multiple of 4
    assert f.call("_Heap_Initialize", heap, area, 0x10, 8) == 0            # below HEAP_MINIMUM_SIZE
    assert f.call("_Heap_Initialize", heap, area, 0x1000, 8) == 0x1000 - 16
    f.u32(0x45C4F4, 3)                                                    # _System_state UP (for get_information)
    a = f.call("_Heap_Allocate", heap, 100)
    b = f.call("_Heap_Allocate", heap, 200)
    assert area < a < area + 0x1000 and area < b < area + 0x1000 and a != b and a % 8 == 0 and b % 8 == 0
    assert f.call("_Heap_Allocate", heap, 0x2000) == 0                    # too big
    sz = f.alloc(4)
    assert f.call("_Heap_Size_of_user_area", heap, a, sz) == 1 and f.u32(sz) >= 100
    info = f.alloc(16)
    assert f.call("_Heap_Get_information", heap, info) == 0
    free_blocks, free_size, used_blocks, used_size = struct.unpack(">4I", f.read(info, 16))
    assert (free_blocks, used_blocks) == (1, 3)                           # the final dummy block counts as used
    assert free_size + used_size == 0x1000 - 8 and b < a                  # blocks come from the top of the free block
    assert f.call("_Heap_Free", heap, a) == 1
    assert f.call("_Heap_Free", heap, area + 0x1800) == 0                 # not in the heap
    assert f.call("_Heap_Get_information", heap, info) == 0
    assert struct.unpack(">4I", f.read(info, 16))[::2] == (2, 2)
    f.u32(0x45C4F4, 2)
    assert f.call("_Heap_Get_information", heap, info) == 1               # not UP: SYSTEM_STATE_ERROR
    amt = f.alloc(4)
    assert f.call("_Heap_Extend", heap, area + 0x100, 0x100, amt) == 1    # inside: HEAP_EXTEND_ERROR
    assert f.call("_Heap_Extend", heap, area + 0x1800, 0x100, amt) == 2   # not contiguous: NOT_IMPLEMENTED
    assert f.call("_Heap_Extend", heap, area + 0x1000, 0x800, amt) == 0 and f.u32(amt) == 0x800
    assert area + 0x1000 <= f.call("_Heap_Allocate", heap, 0x700) < area + 0x1800   # served from the new area


@claim
def rtems_assoc_lookups(f):
    a, b, c = f.cstr("alpha"), f.cstr("beta"), f.cstr("gamma")
    t = f.alloc(48)
    f.write(t, struct.pack(">12I", a, 1, 0x10, b, 2, 0x20, c, 4, 0x40, 0, 0, 0))
    assert f.call("rtems_assoc_ptr_by_name", t, f.cstr("beta")) == t + 12
    assert f.call("rtems_assoc_ptr_by_local", t, 4) == t + 24
    assert f.call("rtems_assoc_ptr_by_remote", t, 0x10) == t
    assert f.call("rtems_assoc_ptr_by_local", t, 9) == 0
    assert f.call("rtems_assoc_remote_by_local", t, 2) == 0x20 and f.call("rtems_assoc_remote_by_local", t, 9) == 0
    assert f.call("rtems_assoc_local_by_remote", t, 0x40) == 4
    assert f.call("rtems_assoc_remote_by_name", t, f.cstr("alpha")) == 0x10
    assert f.call("rtems_assoc_local_by_name", t, f.cstr("gamma")) == 4
    assert f.read_cstr(f.call("rtems_assoc_name_by_local", t, 2)) == "beta"
    assert f.read_cstr(f.call("rtems_assoc_name_by_remote", t, 0x40)) == "gamma"
    assert f.read_cstr(f.call("rtems_assoc_name_by_local", t, 9)) == "<assoc.c: BAD NAME>"
    assert f.read_cstr(f.call("rtems_assoc_name_bad", 9)) == "<assoc.c: BAD NAME>"
    assert f.call("rtems_assoc_remote_by_local_bitfield", t, 5) == 0x50
    assert f.call("rtems_assoc_local_by_remote_bitfield", t, 0x30) == 3
    buf = f.alloc(64)
    assert f.read_cstr(f.call("rtems_assoc_name_by_local_bitfield", t, 3, buf)) == "alpha beta"
    assert f.read_cstr(f.call("rtems_assoc_name_by_remote_bitfield", t, 0x50, buf)) == "alpha gamma"


@claim
def rtems_libio_flag_conversions(f):
    to = lambda x: f.call("rtems_libio_to_fcntl_flags", x)
    assert (to(6), to(2), to(4)) == (2, 0, 1)                             # READ_WRITE -> O_RDWR, READ -> O_RDONLY, WRITE -> O_WRONLY
    assert to(0x200 | 2) == 8 and to(0x400 | 2) == 0x200                  # APPEND -> O_APPEND, CREATE -> O_CREAT
    fr = lambda x: f.call("rtems_libio_fcntl_flags", x)
    assert (fr(0), fr(1), fr(2)) == (2, 4, 6)                             # O_RDONLY/O_WRONLY/O_RDWR -> LIBIO READ/WRITE/both
    assert fr(2 | 8) == 6 | 0x200 and fr(0x200) == 2 | 0x400              # O_APPEND, O_CREAT


@claim
def rtems_semaphore_status_translation(f):
    m = [f.call("_Semaphore_Translate_core_mutex_return_code", i) for i in range(8)]
    assert m == [0, 13, 13, 23, 7, 6, 25, 25], m
    s = [f.call("_Semaphore_Translate_core_semaphore_return_code", i) for i in range(8)]
    assert s == [0, 13, 7, 6, 25, 0, 0, 25], s                            # 5, 6 read past the 5-entry table (source bug)


@claim
def rtems_watchdog_delta_chain(f):
    hdr = f.alloc(12)
    f.write(hdr, struct.pack(">III", hdr + 4, 0, hdr))                    # empty chain
    f.u32(0x45C4B0, 0); f.u32(0x45C4C4, 0); f.u32(0x45C4DC, 0); f.u32(0x45C4E0, 77)
    fired = []
    from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_PC, UC_PPC_REG_LR
    routine = f.alloc(4)
    f.uc.hook_add(1 << 2, lambda uc, a, s, _: (fired.append(uc.reg_read(UC_PPC_REG_3)),
                  uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))), begin=routine, end=routine)
    def wd(ticks, ident):
        w = f.alloc(0x28)                                                 # node, state, initial, delta, start, stop, routine, id, user
        f.write(w, struct.pack(">10I", 0, 0, 0, ticks, 0, 0, 0, routine, ident, 0))
        return w
    w5, w2, w9 = wd(5, 5), wd(2, 2), wd(9, 9)
    for w in (w5, w2, w9):
        f.call("_Watchdog_Insert", hdr, w)
        assert f.u32(w + 8) == 2 and f.u32(w + 0x14) == 77                # ACTIVE, start_time = ticks since boot
    order, n = [], f.u32(hdr)
    while n != hdr + 4:
        order.append((n, f.u32(n + 0x10))); n = f.u32(n)
    assert order == [(w2, 2), (w5, 3), (w9, 4)], order                    # delta list
    assert f.call("_Watchdog_Remove", w5) == 2 and f.u32(w5 + 8) == 0
    assert f.u32(w9 + 0x10) == 7                                          # its delta moved to the next one
    assert f.call("_Watchdog_Remove", w5) == 0                            # already inactive
    for _ in range(9):
        f.call("_Watchdog_Tickle", hdr)
    assert fired == [2, 9], fired


@claim
def imfs_get_token_kinds(f):
    tok, ln = f.alloc(40), f.alloc(4)
    def t(p):
        r = f.call("IMFS_get_token", f.cstr(p), tok, ln)
        return r, f.u32(ln), f.read_cstr(tok)
    assert t("dev/console") == (3, 3, "dev")                              # IMFS_NAME
    assert t("../x") == (2, 2, "..")                                      # IMFS_UP_DIR
    assert t("./x")[0] == 1 and t("/")[:2] == (1, 1)                      # IMFS_CURRENT_DIR
    assert t("")[:2] == (0, 0)                                            # IMFS_NO_MORE_PATH
    assert t("a" * 40)[0] == 4                                            # longer than IMFS_NAME_MAX: INVALID_TOKEN


@claim
def rtems_clock_get_options(f):
    for a, v in ((0x45C760, 2001), (0x45C764, 2), (0x45C768, 3), (0x45C76C, 4), (0x45C770, 5), (0x45C774, 6), (0x45C778, 7)):
        f.u32(a, v)                                                       # _TOD_Current
    f.u32(0x45C4C8, 12345); f.u32(0x45C4E0, 999); f.u32(0x45C478, 100); f.u32(0x45C4F0, 10000)
    out = f.alloc(28)
    f.u32(0x45C490, 0)                                                    # _TOD_Is_set FALSE
    assert f.call("rtems_clock_get", 0, out) == 11 and f.call("rtems_clock_get", 1, out) == 11   # RTEMS_NOT_DEFINED
    assert f.call("rtems_clock_get", 2, out) == 0 and f.u32(out) == 999   # ticks since boot
    assert f.call("rtems_clock_get", 3, out) == 0 and f.u32(out) == 100   # ticks per second
    f.u32(0x45C490, 1)
    assert f.call("rtems_clock_get", 0, out) == 0 and struct.unpack(">7I", f.read(out, 28)) == (2001, 2, 3, 4, 5, 6, 7)
    assert f.call("rtems_clock_get", 1, out) == 0 and f.u32(out) == 12345
    assert f.call("rtems_clock_get", 4, out) == 0 and (f.u32(out), f.u32(out + 4)) == (12345, 70000)
    assert f.call("rtems_clock_get", 5, out) == 25                        # RTEMS_INTERNAL_ERROR


@claim
def bzip2_buff_to_buff_decompress(f):
    from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_PC, UC_PPC_REG_LR
    def mal(uc, a, s, _):
        uc.reg_write(UC_PPC_REG_3, f.alloc(uc.reg_read(UC_PPC_REG_3)))
        uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))
    m = SYM["malloc"]
    f.uc.hook_add(1 << 2, mal, begin=m, end=m)
    f.stub("free", 0)
    plain = bytes(range(256)) * 20 + b"ELITE_RGB bzip2 1.0.2 " * 50
    for small in (0, 1):
        src = f.bytes_(bz2.compress(plain, 1))
        dst, dlen = f.alloc(len(plain) + 16), f.alloc(4)
        f.u32(dlen, len(plain) + 16)
        r = f.call_s("BZ2_bzBuffToBuffDecompress", dst, dlen, src, len(bz2.compress(plain, 1)), small, 0, limit=50_000_000)
        assert r == 0 and f.u32(dlen) == len(plain) and f.read(dst, len(plain)) == plain, (r, f.u32(dlen))
    f.u32(dlen, 100)
    assert f.call_s("BZ2_bzBuffToBuffDecompress", dst, dlen, src, len(bz2.compress(plain, 1)), 0, 0, limit=50_000_000) == -8   # BZ_OUTBUFF_FULL
    assert f.read_cstr(f.call("BZ2_bzlibVersion")) == "1.0.2, 30-Dec-2001"
# ---------------- end ----------------


# ==== library pass (round 15): newlib / libgcc / libm ====
D=lambda v: struct.unpack(">II",struct.pack(">d",v))
W=lambda hi,lo: struct.unpack(">d",struct.pack(">II",hi,lo))[0]

@claim
def zlib_tail(f):
    a=f.bytes_(b"Wikipedia")
    assert f.call("adler32",1,a,9)==0x11E60398
    assert f.read_cstr(f.call("zlibVersion"))=="1.1.4"
    assert f.read_cstr(f.call("zError",-3))=="data error"
    assert f.read_cstr(f.call_s("zError",1)) in ("stream end",)

@claim
def libc_string_more(f):
    d=f.cstr("ab",16); f.call("strncat",d,f.cstr("cdef"),2); assert f.read_cstr(d)=="abcd"
    d=f.alloc(8); f.write(d,b"zzzzzzzz"); f.call("strncpy",d,f.cstr("xy"),5); assert f.read(d,6)==b"xy\0\0\0z"
    s=f.bytes_(b"0123456789"); assert f.call("memchr",s,ord("7"),10)==s+7 and f.call("memchr",s,ord("x"),10)==0
    s=f.bytes_(b"abcdefgh"); f.call("memmove",s+2,s,5); assert f.read(s,8)==b"ababcdeh"
    s=f.bytes_(b"abcdefgh"); f.call("memmove",s,s+2,5); assert f.read(s,8)==b"cdefgfgh"
    a,b=f.cstr("HeLLo"),f.cstr("hellO!")
    assert f.call("strnicmp",a,b,5)==0 and f.call_s("strnicmp",a,b,6)<0
    assert f.call("byte_sum16",f.bytes_(bytes([0xff]*300)),300)==(0xff*300)&0xffff

@claim
def libc_strtok(f):
    s=f.cstr("a,b;;c")
    toks=[]
    p=f.call("strtok",s,f.cstr(",;"))
    while p:
        toks.append(f.read_cstr(p)); p=f.call("strtok",0,f.cstr(",;"))
    assert toks==["a","b","c"],toks
    last=f.alloc(4); s=f.cstr("  x y")
    assert f.read_cstr(f.call("strtok_r",s,f.cstr(" "),last))=="x"
    assert f.read_cstr(f.call("strtok_r",0,f.cstr(" "),last))=="y"

@claim
def libc_strtol(f):
    end=f.alloc(4)
    s=f.cstr("  -0x1Fz")
    assert f.call_s("strtol",s,end,0)==-31 and f.u32(end)==s+7
    assert f.call("strtol",f.cstr("777"),0,8)==511
    assert f.call("strtol",f.cstr("99999999999"),0,10)==0x7fffffff
    assert f.call("atoi",f.cstr(" 42abc"))==42
    assert f.call_s("_strtol_r",f.alloc(0x400),f.cstr("-12"),0,10)==-12

@claim
def libc_div_ctype(f):
    q,r=f.call("div",-7,2,r4=True)
    assert (q-(1<<32),r-(1<<32))==(-3,-1)
    assert f.call("isalpha",ord("q")) and f.call("isalpha",ord("Q")) and not f.call("isalpha",ord("5"))
    assert f.call("isdigit",ord("5")) and not f.call("isdigit",ord("a"))

@claim
def libc_sprintf_r(f):
    buf=f.alloc(64)
    n=f.call("_sprintf_r",f.alloc(0x400),buf,f.cstr("%d-%s-%04x"),-5,f.cstr("ok"),0xbe)
    assert f.read_cstr(buf)=="-5-ok-00be" and n==10, f.read_cstr(buf)

@claim
def libm_bits(f):
    for v,inf,nan in ((1.0,0,0),(float('inf'),1,0),(float('-inf'),1,0),(float('nan'),0,1)):
        assert f.call("isinf",*D(v))==inf and f.call("isnan",*D(v))==nan
    assert W(*f.call("fabs",*D(-2.5),r4=True))==2.5
    assert W(*f.call("copysign",*D(3.0),*D(-1.0),r4=True))==-3.0
    assert W(*f.call("scalbn",*D(1.5),4,r4=True))==24.0
    assert W(*f.call("rint",*D(2.5),r4=True))==2.0 and W(*f.call("rint",*D(-3.7),r4=True))==-4.0
    assert W(*f.call("pow",*D(2.0),*D(10.0),r4=True))==1024.0
    assert W(*f.call("__ieee754_pow",*D(9.0),*D(0.5),r4=True))==3.0
    assert W(*f.call("__ieee754_sqrt",*D(2.0),r4=True))==2.0**0.5
    assert f.call("matherr",0)==0

@claim
def libgcc_shifts_div(f):
    assert f.call("__ashldi3",0,1,40,r4=True)==(0x100,0)
    assert f.call("__lshrdi3",0x80000000,0,36,r4=True)==(0,0x8000000)
    random.seed(2)
    for name in ("__udivmoddi4_divdi3","__udivmoddi4_udivdi3","__udivmoddi4_umoddi3"):
        for _ in range(100):
            n=random.getrandbits(64); d=random.getrandbits(random.choice([8,32,40,64])) or 7
            rp=f.alloc(8)
            hi,lo=f.call(name,n>>32,n&0xffffffff,d>>32,d&0xffffffff,rp,r4=True)
            assert (hi<<32|lo)==n//d and (f.u32(rp)<<32|f.u32(rp+4))==n%d

@claim
def libgcc_fp_parts(f):
    src=f.alloc(8)
    def up(v):
        d=f.alloc(32); f.write(src,struct.pack(">d",v)); f.call("__unpack_d",src,d); return d
    def pk(p): return W(*f.call("__pack_d",p,r4=True))
    assert struct.unpack(">iiiQ",f.read(up(1.0),12)+f.read(up(1.0)+16,8))==(3,0,0,1<<60)  # NORMAL, + , exp 0, 1<<60
    assert f.u32(up(0.0))==2 and f.u32(up(float('inf')))==4 and f.u32(up(float('nan')))==1
    assert pk(f.call("_fpadd_parts",up(1.5),up(2.25),f.alloc(32)))==3.75
    assert pk(f.call("_fpmul_parts",up(1.5),up(2.25),f.alloc(32)))==3.375
    assert pk(f.call("_fpdiv_parts",up(1.5),up(2.25)))==1.5/2.25
    assert f.call_s("__fpcmp_parts_d",up(1.0),up(2.0))==-1 and f.call("__fpcmp_parts_d",up(3.0),up(3.0))==0
    p=f.alloc(16); f.write(p,struct.pack(">iiiI",3,1,1,3<<29)); assert f.call("__pack_f",p)==0xc0400000  # -3.0f
    nan=(0x7ff80000,0)
    for n,want in (("__gtdf2",-1),("__gedf2",-1),("__ltdf2",1)):
        assert f.call_s(n,*nan,*D(1.0))==want and f.call_s(n,*D(1.0),*D(2.0))==-1 and f.call_s(n,*D(2.0),*D(1.0))==1


@claim
def libc_setjmp_longjmp(f):
    jb=f.alloc(0x100)
    assert f.call("setjmp",jb)==0
    assert f.u32(jb+0x54)==fw.STOP and f.u32(jb)==fw.STACK                  # saved LR and r1
    assert f.call("longjmp",jb,7)==7                                         # "returns" from setjmp again
    assert f.call("longjmp",jb,0)==1                                         # val 0 becomes 1

@claim
def libc_fiprintf_has_no_float(f):
    fp,buf=f.alloc(0x5c),f.alloc(64)                                         # string FILE: _p, _r, _w, _flags __SWR|__SSTR
    f.write(fp,struct.pack(">IIIhhII",buf,0,0x7fffffff,0x208,-1,buf,0x7fffffff))
    f.u32(fp+0x54,f.alloc(0x400))                                            # _data (reent)
    assert f.call("fiprintf",fp,f.cstr("%d %f %s"),5,f.cstr("x"))==5
    assert f.read(buf,5)==b"5 f x"                                           # integer-only vfiprintf: %f is not a conversion


@claim
def libc_mprec_helpers(f):
    assert f.call("_hi0bits",0x00010000)==15 and f.call("_hi0bits",0x80000000)==0
    y=f.alloc(4); f.u32(y,0x100)
    assert f.call("_lo0bits",y)==8 and f.u32(y)==1
    assert W(*f.call("_mprec_log10",3,r4=True))==1000.0 and W(*f.call("_mprec_log10",23,r4=True))==1e23
    def big(v):                                                              # struct _Bigint {next, k, maxwds, sign, wds, x[]}
        b=f.alloc(32); f.write(b,struct.pack(">iiiiiI",0,1,2,0,1,v)); return b
    b,S=big(123),big(20)
    assert f.call("__mcmp",b,S)==1 and f.call_s("__mcmp",S,b)==-1
    assert f.call("dtoa_quorem",b,S)==6 and f.u32(b+20)==3                       # quotient digit; b becomes b % S
    buf=f.alloc(16)
    assert f.call("vfprintf_exponent",buf,5,ord("e"))==4 and f.read(buf,4)==b"e+05"   # vfprintf's exponent()
    assert f.call("vfprintf_exponent",buf,-123,ord("E"))==5 and f.read(buf,5)==b"E-123"

@claim
def libc_locale(f):
    assert f.read_cstr(f.call("setlocale",0,f.cstr("C")))=="C"
    assert f.call("setlocale",0,f.cstr("fr_FR"))==0
    assert f.read_cstr(f.u32(f.call("localeconv")))=="."                    # lconv.decimal_point


@claim
def app_helpers_b2xxx(f):
    f.u32(0xB4C74,10)
    f.call("spin_delay",1000)
    assert f.u32(0xB4C74)==1010                                              # bumps the counter once per count
    f.call("spin_delay",0); assert f.u32(0xB4C74)==1010



# ==== library pass (round 15): libpng / zlib / bzip2 ====
# ---- libpng 1.2.5 / zlib 1.1.4 / bzip2 1.0.2 (library pass) ----
# paste into analysis/tools/test_claims.py; needs: import zlib; from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_5, UC_PPC_REG_PC, UC_PPC_REG_LR
def heap_hooks(f):
    """malloc/free -> Python bump allocator (RTEMS heap is not initialised outside the firmware)."""
    def _malloc(uc, addr, size, _):
        n = uc.reg_read(UC_PPC_REG_3)
        uc.reg_write(UC_PPC_REG_3, f.alloc(max(n, 1)))
        uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))
    a = f.addr("malloc"); f.uc.hook_add(1 << 2, _malloc, begin=a, end=a)
    f.stub("free", 0)

def zalloc_fn(f):
    """a fake function address for z_stream.zalloc(opaque, items, size)"""
    A = 0x1E00100
    f.write(A, b"\x4e\x80\x00\x20" * 2)
    def h(uc, addr, size, _):
        n = uc.reg_read(UC_PPC_REG_4) * uc.reg_read(UC_PPC_REG_5)
        uc.reg_write(UC_PPC_REG_3, f.alloc(max(n, 1)))
        uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))
    f.uc.hook_add(1 << 2, h, begin=A, end=A)
    return A, 0x1E00104        # zfree: a bare blr

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

def make_png(w, h, rows, extra=b""):
    raw = b"".join(b"\0" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + extra
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

# ---------------- zlib ----------------
@claim
def zlib_crc32_adler32(f):
    for data in (b"", b"a", b"hello world" * 50, bytes(range(256)) * 30):
        p = f.bytes_(data)
        assert f.call("crc32", 0, p, len(data)) == zlib.crc32(data) & 0xFFFFFFFF
        assert f.call("adler32", 1, p, len(data)) == zlib.adler32(data) & 0xFFFFFFFF, data[:8]
    assert f.call("adler32", 1, 0, 0) == 1                            # buf NULL -> initial value
    t = f.call("get_crc_table")
    assert f.u32(t + 4) == 0x77073096                                 # standard table entry 1
    assert f.read_cstr(f.call("zlibVersion")) == "1.1.4"
    assert f.read_cstr(f.call("zError", (-3) & 0xFFFFFFFF)) == "data error"
    assert f.read_cstr(f.call("zError", 2)) == "need dictionary"

@claim
def zlib_inflate_stream(f):
    za, zf = zalloc_fn(f)
    import random
    rnd = random.Random(3)
    data = (b"The quick brown fox jumps over the lazy dog. " * 40) + bytes(rnd.choice(b"etaoin shrdlu") for _ in range(3000))
    for level in (0, 1, 9):
        comp = zlib.compress(data, level)
        zs = f.alloc(0x38)
        out = f.alloc(len(data) + 16)
        f.write(zs, struct.pack(">IIIIII", f.bytes_(comp), len(comp), 0, out, len(data) + 16, 0))
        f.u32(zs + 0x20, za); f.u32(zs + 0x24, zf); f.u32(zs + 0x28, 0)
        assert f.call("inflateInit_", zs, f.cstr("1.1.4"), 0x38) == 0
        assert f.call("inflate", zs, 4) == 1                          # Z_FINISH -> Z_STREAM_END
        assert f.u32(zs + 0x14) == len(data) and f.read(out, len(data)) == data
        assert f.call("inflateReset", zs) == 0 and f.u32(zs + 0x14) == 0
        assert f.call("inflateEnd", zs) == 0 and f.u32(zs + 0x1c) == 0
    zs = f.alloc(0x38)
    f.u32(zs + 0x20, za); f.u32(zs + 0x24, zf)
    assert f.call_s("inflateInit_", zs, f.cstr("2.0"), 0x38) == -6       # Z_VERSION_ERROR
    assert f.call_s("inflateInit2_", zs, 7, f.cstr("1.1.4"), 0x38) == -2 # window bits < 8: Z_STREAM_ERROR

@claim
def zlib_uncompress_raw_window(f):
    # uncompress uses zcalloc (calloc) -> hook calloc via the zalloc path is not possible; use inflateInit2_ with -15
    za, zf = zalloc_fn(f)
    data = b"raw deflate " * 30
    co = zlib.compressobj(6, zlib.DEFLATED, -15); comp = co.compress(data) + co.flush()
    zs = f.alloc(0x38); out = f.alloc(len(data) + 8)
    f.write(zs, struct.pack(">IIIIII", f.bytes_(comp), len(comp), 0, out, len(data) + 8, 0))
    f.u32(zs + 0x20, za); f.u32(zs + 0x24, zf)
    assert f.call("inflateInit2_", zs, (-15) & 0xFFFFFFFF, f.cstr("1.1.4"), 0x38) == 0
    assert f.call("inflate", zs, 4) == 1 and f.read(out, len(data)) == data

@claim
def zlib_uncompress(f):
    # uncompress(dest, &destLen, src, srcLen) with zalloc = 0 -> zcalloc -> calloc (os_00062b54): hook it
    def _calloc(uc, addr, size, _):
        n = uc.reg_read(UC_PPC_REG_3) * uc.reg_read(UC_PPC_REG_4)
        uc.reg_write(UC_PPC_REG_3, f.alloc(max(n, 1)))
        uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))
    f.uc.hook_add(1 << 2, _calloc, begin=0x62b54, end=0x62b54)
    f.stub("free", 0)
    data = b"uncompress me please " * 20
    comp = zlib.compress(data)
    dst = f.alloc(1024); ln = f.alloc(4); f.u32(ln, 1024)
    assert f.call("uncompress", dst, ln, f.bytes_(comp), len(comp)) == 0
    assert f.u32(ln) == len(data) and f.read(dst, len(data)) == data
    f.u32(ln, 10)
    assert f.call_s("uncompress", dst, ln, f.bytes_(comp), len(comp)) == -5   # Z_BUF_ERROR: dest too small

# ---------------- libpng helpers ----------------
@claim
def png_sig_and_bytes(f):
    sig = f.bytes_(b"\x89PNG\r\n\x1a\n")
    assert f.call("png_sig_cmp", sig, 0, 8) == 0
    assert f.call("png_sig_cmp", f.bytes_(b"\x89PNX\r\n\x1a\n"), 0, 8) != 0
    assert f.call("png_sig_cmp", f.bytes_(b"\x89PNX\r\n\x1a\n"), 0, 3) == 0   # only the first 3 compared
    assert f.call("png_check_sig", sig, 8) == 1 and f.call("png_check_sig", f.bytes_(b"GIF89a.."), 8) == 0
    p = f.bytes_(b"\x80\x01\x02\x03\xfe\xdc")
    assert f.call("png_get_uint_32", p) == 0x80010203
    assert f.call_s("png_get_int_32", p) == -0x7FFEFDFD
    assert f.call("png_get_uint_16", p + 4) == 0xFEDC
    assert f.call("png_access_version_number") == 10205
    assert f.read_cstr(f.call("png_get_libpng_ver", 0)) == "1.2.5" == f.read_cstr(f.call("png_get_header_ver", 0))
    assert "1.2.5" in f.read_cstr(f.call("png_get_header_version", 0))
    assert f.call_s("png_mmx_support") == -1

@claim
def png_info_setters_getters(f):
    png = f.alloc(0x2e0); info = f.alloc(0xec)
    f.call("png_set_oFFs", png, info, 100, (-5) & 0xFFFFFFFF, 1)
    f.call("png_set_pHYs", png, info, 2835, 2834, 1)
    tm = f.bytes_(bytes([0x07, 0xD2, 10, 31, 23, 59, 58, 0]))           # png_time {u16 year; u8 month, day, hour, minute, second}
    f.call("png_set_tIME", png, info, tm)
    v = f.call("png_get_valid", png, info, 0xFFFFFFFF)
    assert v == 0x100 | 0x80 | 0x200, hex(v)                            # PNG_INFO_oFFs | pHYs | tIME
    assert f.call("png_get_x_offset_microns", png, info) == 100
    assert f.call_s("png_get_y_offset_microns", png, info) == -5
    assert f.call("png_get_x_offset_pixels", png, info) == 0            # unit is micrometres, not pixels
    assert f.call("png_get_x_pixels_per_meter", png, info) == 2835 and f.call("png_get_y_pixels_per_meter", png, info) == 2834
    assert f.call("png_get_pixels_per_meter", png, info) == 0           # x != y
    a, b, c = f.alloc(4), f.alloc(4), f.alloc(4)
    assert f.call("png_get_oFFs", png, info, a, b, c) == 0x100 and (f.u32(a), f.u32(c)) == (100, 1)
    assert f.call("png_get_pHYs", png, info, a, b, c) == 0x80 and (f.u32(a), f.u32(b), f.u32(c)) == (2835, 2834, 1)
    assert f.call("png_get_tIME", png, info, a) == 0x200 and f.read(f.u32(a), 7) == bytes([0x07, 0xD2, 10, 31, 23, 59, 58])
    f.call("png_set_invalid", png, info, 0x80)
    assert f.call("png_get_valid", png, info, 0x80) == 0 and f.call("png_get_pHYs", png, info, a, b, c) == 0
    f.call("png_set_mmx_thresholds", png, 9, 1234)
    assert f.call("png_get_mmx_bitdepth_threshold", png) == 9 and f.call("png_get_mmx_rowbytes_threshold", png) == 1234
    f.call("png_set_read_user_chunk_fn", png, 0x1234, 0x5678)
    assert f.call("png_get_user_chunk_ptr", png) == 0x1234
    f.call("png_set_error_fn", png, 0xABCD, 0, 0)
    assert f.call("png_get_error_ptr", png) == 0xABCD

@claim
def png_transform_setters(f):
    png = f.alloc(0x2e0)
    T = 0x124
    for name, bit in (("png_set_bgr", 0x1), ("png_set_swap_alpha", 0x20000), ("png_set_invert_alpha", 0x80000),
                      ("png_set_invert_mono", 0x20), ("png_set_strip_16", 0x400), ("png_set_strip_alpha", 0x40000),
                      ("png_set_expand", 0x1000), ("png_set_palette_to_rgb", 0x1000), ("png_set_gray_1_2_4_to_8", 0x1000),
                      ("png_set_tRNS_to_alpha", 0x1000), ("png_set_gray_to_rgb", 0x4000)):
        f.u32(png + T, 0)
        f.call(name, png)
        assert f.u32(png + T) == bit, (name, hex(f.u32(png + T)))
    f.u32(png + T, 0); f.write(png + 0x1db, b"\x10")                    # bit_depth 16
    f.call("png_set_swap", png); assert f.u32(png + T) == 0x10
    f.u32(png + T, 0); f.write(png + 0x1db, b"\x04")
    f.call("png_set_packing", png); assert f.u32(png + T) == 0x4
    f.u32(png + T, 0); f.call("png_set_packswap", png); assert f.u32(png + T) == 0x10000
    f.u32(png + T, 0); f.call("png_set_swap", png); assert f.u32(png + T) == 0      # only for 16-bit
    f.u32(png + T, 0); f.call("png_set_filler", png, 0xFF, 1); assert f.u32(png + T) == 0x8000
    f.u32(png + T, 0); f.call("png_set_read_user_transform_fn", png, 0x4444)
    assert f.u32(png + T) == 0x100000 and f.u32(png + 0x118) == 0x4444
    for action, flags in ((3, 0x400), (4, 0xC00), (1, 0)):              # crit_action: USE / NO_CHANGE(ignore) / DEFAULT
        f.u32(png + 0x120, 0)
        f.call("png_set_crc_action", png, action, 5)                    # ancil 5 = no change
        assert f.u32(png + 0x120) & 0xC00 == flags, (action, hex(f.u32(png + 0x120)))

@claim
def png_do_row_transforms(f):
    def ri(width, ct, bd, ch):
        r = f.alloc(12)
        pix = bd * ch
        f.write(r, struct.pack(">II", width, (width * pix + 7) // 8) + bytes([ct, bd, ch, pix]))
        return r
    # png_do_bgr: RGB 8-bit swaps R and B
    r = ri(2, 2, 8, 3); row = f.bytes_(bytes([1, 2, 3, 4, 5, 6]))
    f.call("png_do_bgr", r, row); assert f.read(row, 6) == bytes([3, 2, 1, 6, 5, 4])
    # png_do_invert: gray 8-bit inverted
    r = ri(3, 0, 8, 1); row = f.bytes_(bytes([0, 0x0F, 0xFF]))
    f.call("png_do_invert", r, row); assert f.read(row, 3) == bytes([0xFF, 0xF0, 0])
    # png_do_swap: 16-bit samples byte-swapped
    r = ri(2, 0, 16, 1); row = f.bytes_(bytes([1, 2, 3, 4]))
    f.call("png_do_swap", r, row); assert f.read(row, 4) == bytes([2, 1, 4, 3])
    # png_do_chop: 16 -> 8 keeps the high bytes
    r = ri(2, 0, 16, 1); row = f.bytes_(bytes([0xAB, 0xCD, 0x12, 0x34]))
    f.call("png_do_chop", r, row); assert f.read(row, 2) == bytes([0xAB, 0x12]) and f.read(r + 9, 1) == b"\x08"
    # png_do_unpack: 1-bit gray -> one byte per pixel
    r = ri(8, 0, 1, 1); row = f.bytes_(bytes([0b10110001]) + bytes(8))
    f.call("png_do_unpack", r, row); assert f.read(row, 8) == bytes([1, 0, 1, 1, 0, 0, 0, 1])
    # png_do_packswap: 4-bit pixels swapped within the byte
    r = ri(4, 0, 4, 1); row = f.bytes_(bytes([0x12, 0x34]))
    f.call("png_do_packswap", r, row); assert f.read(row, 2) == bytes([0x21, 0x43])
    # png_do_gray_to_rgb: gray 8 -> RGB
    r = ri(2, 0, 8, 1); row = f.bytes_(bytes([7, 9]) + bytes(8))
    f.call("png_do_gray_to_rgb", r, row); assert f.read(row, 6) == bytes([7, 7, 7, 9, 9, 9]) and f.read(r + 8, 1) == b"\x02"
    # png_do_read_swap_alpha: RGBA -> ARGB
    r = ri(1, 6, 8, 4); row = f.bytes_(bytes([1, 2, 3, 4]))
    f.call("png_do_read_swap_alpha", r, row); assert f.read(row, 4) == bytes([4, 1, 2, 3])
    # png_do_read_invert_alpha
    r = ri(1, 6, 8, 4); row = f.bytes_(bytes([1, 2, 3, 0x10]))
    f.call("png_do_read_invert_alpha", r, row); assert f.read(row, 4) == bytes([1, 2, 3, 0xEF])
    # png_do_strip_filler: RGBA -> RGB with flag PNG_FLAG_FILLER_AFTER (0x80)
    r = ri(2, 6, 8, 4); row = f.bytes_(bytes([1, 2, 3, 0xFF, 4, 5, 6, 0xFF]))
    f.call("png_do_strip_filler", r, row, 0x80); assert f.read(row, 6) == bytes([1, 2, 3, 4, 5, 6])
    # png_do_read_filler: RGB 8 -> RGBX (filler after: flags 0x80)
    r = ri(2, 2, 8, 3); row = f.bytes_(bytes([1, 2, 3, 4, 5, 6]) + bytes(4))
    f.call("png_do_read_filler", r, row, 0xEE, 0x80); assert f.read(row, 8) == bytes([1, 2, 3, 0xEE, 4, 5, 6, 0xEE])
    # png_build_grayscale_palette(bit_depth 2, palette): 4 grey levels 0, 0x55, 0xAA, 0xFF
    pal = f.alloc(12)
    f.call("png_build_grayscale_palette", 2, pal)
    assert f.read(pal, 12) == bytes(sum(([v] * 3 for v in (0, 0x55, 0xAA, 0xFF)), []))

@claim
def png_decode_memory(f):
    heap_hooks(f)
    rows = [bytes([10, 20, 30, 40, 50, 60, 70, 80, 90]), bytes([1, 2, 3, 4, 5, 6, 7, 8, 9])]
    extra = (chunk(b"pHYs", struct.pack(">IIB", 3780, 3780, 1)) + chunk(b"oFFs", struct.pack(">iiB", 7, -3, 0))
             + chunk(b"tIME", struct.pack(">HBBBBB", 2002, 10, 3, 12, 0, 0)) + chunk(b"tEXt", b"Title\0Hello")
             + chunk(b"gAMA", struct.pack(">I", 45455)) + chunk(b"sBIT", bytes([5, 6, 5])))
    data = make_png(3, 2, rows, extra)
    png = f.call("png_create_read_struct", f.cstr("1.2.5"), 0, 0, 0)
    assert png
    info = f.call("png_create_info_struct", png)
    st = f.alloc(12); f.write(st, struct.pack(">III", f.bytes_(data), 0, len(data)))
    f.call("png_set_read_fn", png, st, f.addr("png_read_cb_mem"))
    assert f.call("png_get_io_ptr", png) == st
    f.call("png_read_info", png, info)
    assert f.call("png_get_image_width", png, info) == 3 and f.call("png_get_image_height", png, info) == 2
    assert f.call("png_get_bit_depth", png, info) == 8 and f.call("png_get_color_type", png, info) == 2
    assert f.call("png_get_channels", png, info) == 3 and f.call("png_get_rowbytes", png, info) == 9
    assert f.call("png_get_interlace_type", png, info) == 0 and f.call("png_get_compression_type", png, info) == 0
    assert f.call("png_get_filter_type", png, info) == 0
    assert f.read(f.call("png_get_signature", png, info), 8) == b"\x89PNG\r\n\x1a\n"
    assert f.call("png_get_pixels_per_meter", png, info) == 3780
    assert f.call("png_get_x_offset_pixels", png, info) == 7 and f.call_s("png_get_y_offset_pixels", png, info) == -3
    g = f.alloc(4)
    assert f.call("png_get_gAMA_fixed", png, info, g) == 1 and f.u32(g) == 45455
    assert f.call("png_get_sBIT", png, info, g) == 2 and f.read(f.u32(g), 3) == bytes([5, 6, 5])
    tp, nt = f.alloc(4), f.alloc(4)
    assert f.call("png_get_text", png, info, tp, nt) == 1 and f.u32(nt) == 1
    t0 = f.u32(tp)                                                       # png_text {int compression; char *key; char *text; ...}
    assert f.read_cstr(f.u32(t0 + 4)) == "Title" and f.read_cstr(f.u32(t0 + 8)) == "Hello"
    assert f.call("png_get_tIME", png, info, g) == 0x200 and f.read(f.u32(g), 4) == struct.pack(">HBB", 2002, 10, 3)
    rp = f.alloc(8); bufs = [f.alloc(16), f.alloc(16)]
    f.u32(rp, bufs[0]); f.u32(rp + 4, bufs[1])
    f.call("png_read_image", png, rp)
    assert [f.read(b, 9) for b in bufs] == rows
    f.call("png_read_end", png, info)
    pp, ip = f.alloc(4), f.alloc(4); f.u32(pp, png); f.u32(ip, info)
    f.call("png_destroy_read_struct", pp, ip, 0)
    assert f.u32(pp) == 0 and f.u32(ip) == 0

@claim
def png_decode_with_transforms(f):
    heap_hooks(f)
    rows = [bytes([10, 20, 30, 40, 50, 60]), bytes([1, 2, 3, 4, 5, 6])]
    data = make_png(2, 2, rows)
    png = f.call("png_create_read_struct", f.cstr("1.2.5"), 0, 0, 0)
    info = f.call("png_create_info_struct", png)
    st = f.alloc(12); f.write(st, struct.pack(">III", f.bytes_(data), 0, len(data)))
    f.call("png_set_read_fn", png, st, f.addr("png_read_cb_mem"))
    f.call("png_read_info", png, info)
    f.call("png_set_bgr", png); f.call("png_set_filler", png, 0xAA, 1)
    f.call("png_read_update_info", png, info)
    assert f.call("png_get_rowbytes", png, info) == 8 and f.call("png_get_channels", png, info) == 4
    rp = f.alloc(8); bufs = [f.alloc(16), f.alloc(16)]
    f.u32(rp, bufs[0]); f.u32(rp + 4, bufs[1])
    f.call("png_read_rows", png, rp, 0, 2)                               # png_read_rows(png, row, display, n)
    assert f.read(bufs[0], 8) == bytes([30, 20, 10, 0xAA, 60, 50, 40, 0xAA]), f.read(bufs[0], 8).hex()
    assert f.read(bufs[1], 8) == bytes([3, 2, 1, 0xAA, 6, 5, 4, 0xAA])

@claim
def png_bad_crc_is_an_error(f):
    heap_hooks(f)
    data = bytearray(make_png(1, 1, [bytes([1, 2, 3])]))
    data[29] ^= 0xFF                                                     # IHDR CRC byte
    errs = []
    A = 0x1E00200; f.write(A, b"\x4e\x80\x00\x20")
    def h(uc, addr, size, _):                                            # user error_fn: record and stop the run
        errs.append(f.read_cstr(uc.reg_read(UC_PPC_REG_4)))
        uc.emu_stop()
    f.uc.hook_add(1 << 2, h, begin=A, end=A)
    png = f.call("png_create_read_struct", f.cstr("1.2.5"), 0, A, 0)
    info = f.call("png_create_info_struct", png)
    st = f.alloc(12); f.write(st, struct.pack(">III", f.bytes_(bytes(data)), 0, len(data)))
    f.call("png_set_read_fn", png, st, f.addr("png_read_cb_mem"))
    try:
        f.call("png_read_info", png, info)
    except RuntimeError:
        pass
    assert errs and "CRC error" in errs[0], errs

@claim
def zlib_dictionary_and_sync(f):
    za, zf = zalloc_fn(f)
    dic = b"marquee sign jackpot value "
    data = b"jackpot value marquee sign " * 8
    co = zlib.compressobj(9, zlib.DEFLATED, 15, 9, zlib.Z_DEFAULT_STRATEGY, dic)
    comp = co.compress(data) + co.flush()
    def stream(comp, outn):
        zs = f.alloc(0x38); out = f.alloc(outn)
        f.write(zs, struct.pack(">IIIIII", f.bytes_(comp), len(comp), 0, out, outn, 0))
        f.u32(zs + 0x20, za); f.u32(zs + 0x24, zf)
        assert f.call("inflateInit_", zs, f.cstr("1.1.4"), 0x38) == 0
        return zs, out
    zs, out = stream(comp, 512)
    assert f.call("inflate", zs, 0) == 2                                # Z_NEED_DICT
    assert f.u32(zs + 0x30) == zlib.adler32(dic) & 0xFFFFFFFF          # adler = the dictionary id
    assert f.call_s("inflateSetDictionary", zs, f.bytes_(b"wrong"), 5) == -3   # Z_DATA_ERROR: wrong dictionary
    assert f.call("inflateSetDictionary", zs, f.bytes_(dic), len(dic)) == 0
    assert f.call("inflate", zs, 4) == 1 and f.read(out, len(data)) == data
    # inflateSync: skip a damaged start up to the next full-flush point (00 00 FF FF)
    co = zlib.compressobj(6)
    part1 = co.compress(b"A" * 100) + co.flush(zlib.Z_FULL_FLUSH)
    part2 = co.compress(b"second part " * 5) + co.flush(zlib.Z_SYNC_FLUSH)
    bad = b"\x78\x9c" + b"\xff" * 20 + part1[-4:] + part2
    zs, out = stream(bad, 512)
    assert f.call_s("inflate", zs, 2) == -3                           # Z_DATA_ERROR on the junk
    assert f.call("inflateSync", zs) == 0
    f.call("inflate", zs, 2)
    assert f.read(out, 60) == b"second part " * 5
    assert f.call("inflateSyncPoint", zs) in (0, 1)

@claim
def png_unknown_chunk_lists(f):
    heap_hooks(f)
    png = f.call("png_create_read_struct", f.cstr("1.2.5"), 0, 0, 0)
    f.call("png_set_keep_unknown_chunks", png, 3, f.bytes_(b"abCD\0prVt\0"), 2)   # keep "always" for two chunks
    assert f.u32(png + 0x2a0) == 2
    assert f.call("png_handle_as_unknown", png, f.bytes_(b"prVt")) == 3
    assert f.call("png_handle_as_unknown", png, f.bytes_(b"zzZZ")) == 0
    info = f.call("png_create_info_struct", png)
    f.call("png_set_sCAL_s", png, info, 1, f.cstr("1.5"), f.cstr("2.25"))
    u, w, h = f.alloc(4), f.alloc(4), f.alloc(4)
    assert f.call("png_get_sCAL_s", png, info, u, w, h) == 0x4000
    assert f.u32(u) == 1 and f.read_cstr(f.u32(w)) == "1.5" and f.read_cstr(f.u32(h)) == "2.25"
    f.call("png_set_compression_buffer_size", png, 0x400)
    assert f.call("png_get_compression_buffer_size", png) == 0x400
    p = f.call("png_malloc", png, 40)
    assert p and f.call("png_get_mem_ptr", png) == 0

@claim
def bzip2_buffer_roundtrip(f):
    import bz2, random
    heap_hooks(f)
    assert f.read_cstr(f.call("BZ2_bzlibVersion")) == "1.0.2, 30-Dec-2001"
    rnd = random.Random(7)
    small = bytes(rnd.choice(b"abcde ") for _ in range(3000))            # < 10000 bytes: fallbackSort
    big = (b"jackpot " * 400) + bytes(rnd.randrange(256) for _ in range(20000))   # mainSort
    for data in (small, big, b"x" * 15000):
        comp = bz2.compress(data, 1)
        for small_mode in (0, 1):                                         # FAST and SMALL output paths
            dst = f.alloc(len(data) + 64); ln = f.alloc(4); f.u32(ln, len(data) + 64)
            assert f.call("BZ2_bzBuffToBuffDecompress", dst, ln, f.bytes_(comp), len(comp), small_mode, 0, limit=200_000_000) == 0
            assert f.u32(ln) == len(data) and f.read(dst, len(data)) == data
        dst = f.alloc(len(data) * 2 + 600); ln = f.alloc(4); f.u32(ln, len(data) * 2 + 600)
        assert f.call("BZ2_bzBuffToBuffCompress", dst, ln, f.bytes_(data), len(data), 1, 0, 30, limit=400_000_000) == 0
        assert bz2.decompress(f.read(dst, f.u32(ln))) == data              # the firmware's bzip2 output is valid
    bad = bytearray(bz2.compress(small)); bad[20] ^= 0x55
    dst = f.alloc(4000); ln = f.alloc(4); f.u32(ln, 4000)
    assert f.call_s("BZ2_bzBuffToBuffDecompress", dst, ln, f.bytes_(bytes(bad)), len(bad), 0, 0, limit=200_000_000) in (-4, -5)  # DATA_ERROR

@claim
def png_palette_read_png(f):
    heap_hooks(f)
    w, h = 4, 2
    pal = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9])
    raw = b"".join(b"\0" + r for r in (bytes([0, 1, 2, 3]), bytes([3, 2, 1, 0])))
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
            + chunk(b"sRGB", b"\x01") + chunk(b"PLTE", pal) + chunk(b"tRNS", bytes([0x80, 0xFF]))
            + chunk(b"bKGD", b"\x02") + chunk(b"hIST", struct.pack(">4H", 1, 2, 3, 4))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    png = f.call("png_create_read_struct", f.cstr("1.2.5"), 0, 0, 0)
    info = f.call("png_create_info_struct", png)
    st = f.alloc(12); f.write(st, struct.pack(">III", f.bytes_(data), 0, len(data)))
    f.call("png_set_read_fn", png, st, f.addr("png_read_cb_mem"))
    f.call("png_read_png", png, info, 0x10, 0)                           # PNG_TRANSFORM_EXPAND
    rows = f.call("png_get_rows", png, info)
    got = [f.read(f.u32(rows + 4 * y), w * 4) for y in range(h)]
    rgba = lambda i: pal[3 * i:3 * i + 3] + bytes([[0x80, 0xFF, 0xFF, 0xFF][i]])
    assert got[0] == b"".join(rgba(i) for i in (0, 1, 2, 3)), got[0].hex()   # palette + tRNS -> RGBA
    assert got[1] == b"".join(rgba(i) for i in (3, 2, 1, 0))
    a, b = f.alloc(4), f.alloc(4)
    assert f.call("png_get_PLTE", png, info, a, b) == 8 and f.u32(b) == 4 and f.read(f.u32(a), 12) == pal
    c = f.alloc(4)
    assert f.call("png_get_tRNS", png, info, a, b, c) == 0x10            # valid; info now describes RGBA, so
    assert f.u32(a) == 0 and f.u32(c) == info + 0x4c                     # no palette alpha list, trans_values instead
    assert f.call("png_get_bKGD", png, info, a) == 0x20 and f.read(f.u32(a), 1) == b"\x02"   # png_color_16.index
    assert f.call("png_get_hIST", png, info, a) == 0x40 and f.read(f.u32(a), 8) == struct.pack(">4H", 1, 2, 3, 4)
    assert f.call("png_get_sRGB", png, info, a) == 0x800 and f.u32(a) == 1

@claim
def png_shift_and_rgb_to_gray(f):
    heap_hooks(f)
    rows = [bytes([0xF8, 0xFC, 0xF8, 0x08, 0x04, 0x10])]
    data = make_png(2, 1, rows, chunk(b"sBIT", bytes([5, 6, 5])))
    def start():
        png = f.call("png_create_read_struct", f.cstr("1.2.5"), 0, 0, 0)
        info = f.call("png_create_info_struct", png)
        st = f.alloc(12); f.write(st, struct.pack(">III", f.bytes_(data), 0, len(data)))
        f.call("png_set_read_fn", png, st, f.addr("png_read_cb_mem"))
        f.call("png_read_info", png, info)
        return png, info
    png, info = start()
    sb = f.alloc(4)
    assert f.call("png_get_sBIT", png, info, sb) == 2
    f.call("png_set_shift", png, f.u32(sb))                              # undo the sBIT scaling
    f.call("png_read_update_info", png, info)
    out = f.alloc(8); f.call("png_read_row", png, out, 0)
    assert f.read(out, 6) == bytes([0x1F, 0x3F, 0x1F, 0x01, 0x01, 0x02]), f.read(out, 6).hex()
    png, info = start()
    f.call("png_set_rgb_to_gray_fixed", png, 1, 0xFFFFFFFF, 0xFFFFFFFF)  # error_action 1 (none), default weights
    f.call("png_read_update_info", png, info)
    assert f.call("png_get_channels", png, info) == 1
    out = f.alloc(8); f.call("png_read_row", png, out, 0)
    g = f.read(out, 2)
    assert 0xF8 <= g[0] <= 0xFC and 0x05 <= g[1] <= 0x08, g.hex()       # 0.212R + 0.715G + 0.072B
    assert f.call("png_get_rgb_to_gray_status", png) == 1                # the pixels were not grey


@claim
def lfs_volume_append_unpacked_all_codecs(f0):
    plain = b"Hello LFS world! " * 20

    def entry(name, data, size, flags):
        nb = name.encode()
        head = struct.pack(">III", 0, size, flags) + bytes([len(nb)]) + nb
        head += b"\0" * (-len(head) % 4)
        e = head + data
        e += b"\0" * (-len(e) % 4) + b"\0" * 4
        return struct.pack(">I", len(e)) + e[4:]
    for flags, packed in ((0, plain), (0x40000000, zlib.compress(plain)), (0x20000000, nrv2d_encode(plain)),
                          (0x10000000, bz2.compress(plain, 1))):
        f = FW()
        heap_hooks(f)
        dst = f.alloc(0x1000)                                            # below the source (NRV2D unpacks in place)
        vol = entry("theme.ini", packed, len(plain), flags | 1) + bytes(16)
        base = f.bytes_(vol)
        h, out = f.alloc(0x40), f.alloc(4)
        assert f.call("lfs_find_in_volume", h, base, len(vol), f.cstr("theme.ini")) == 1
        assert f.call("lfs_volume_append_unpacked", h, dst, 0x1000, 1, out, limit=200_000_000) == 1, hex(flags)
        n = f.u32(dst)                                                   # entry length; the chain ends after it
        assert n == 0x18 + len(plain) + 4 and f.u32(dst + n) == 0 and f.u32(out) == len(plain)
        assert f.u32(dst + 4) == len(plain) and f.u32(dst + 8) == 0      # flags: codec bits and bit 0 cleared
        assert f.read(dst + 0xC, 10) == b"\x09theme.ini" and f.read(dst + 0x18, len(plain)) == plain
        assert f.u32(dst + n - 4) == zlib.crc32(f.read(dst, n - 4))      # CRC-32 of the whole entry


@claim
def small_app_setters(f):
    for a, v in ((0xE2D64, 0x11112222), (0xE2D6C, 0x33334444), (0xE2D70, 0x55556666), (0xE2D74, 0x77778888)):
        f.u32(a, v)
    f.u16(0xE2D6A, 0xBEEF)                                               # also the low half of 0xE2D68
    f.call("colour_broadcast_e2d6a")
    assert [f.u32(a) for a in (0xE2D64, 0xE2D6C, 0xE2D70, 0xE2D74)] == [0x1111BEEF, 0x3333BEEF, 0x5555BEEF, 0x7777BEEF]
    f.write(0xB30C0, b"\xff" * 32)
    f.write(0xDCEC0, b"\x05")                                            # 5 configured levels
    f.call("level_flags_clear")
    assert f.read(0xB30C0, 32) == b"\0" * 20 + b"\xff" * 12            # one word per level
    f.call("display_select", 42, 0x1234)
    assert (f.u32(0xB30A8), f.u32(0xB30AC)) == (42, 0x1234)
    f.call("display_select", 100, 7)                                     # n >= 100 is ignored
    assert (f.u32(0xB30A8), f.u32(0xB30AC)) == (42, 0x1234)


def main():
    verbose = "-v" in sys.argv
    pick = [a for a in sys.argv[1:] if a != "-v"]
    ok = 0
    bad = []
    for c in CLAIMS:
        if pick and not any(p in c.__name__ for p in pick):
            continue
        try:
            c(FW())
            ok += 1
            if verbose:
                print(f"  ok   {c.__name__}")
        except Exception as e:
            bad.append(c.__name__)
            print(f"  FAIL {c.__name__}: {type(e).__name__} {e}")
    print(f"{ok}/{ok + len(bad)} claims hold" + ("" if not bad else f"; failing: {', '.join(bad)}"))
    print("ALL CLAIMS HOLD" if not bad else "CLAIMS FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
