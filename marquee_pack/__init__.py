"""marquee_pack: the content/asset toolchain shared by the firmware build and the Studio.

It packs the bytes that go onto the sign and into the firmware image (lfs, signproto, native_emoji, mqf,
font_import, gif2flc, glow, sign_emu). It depends on nothing in studio/, so the firmware build never does.
"""
