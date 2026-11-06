/* Detour for font_draw_char (ELITE_RGB 0x42DB0). The bootloader replaces the first instruction there
 * (stwu r1,-144(r1)) with "b emoji_hook". Arguments arrive unchanged: r3 frame, r4 pos, r5 font,
 * r6 character, r7 fg, r8 bg. */
	.text
	.globl	emoji_hook
emoji_hook:
	cmplwi	%r6, 0x80		/* only codes 0x80..0xFF can be colour emoji */
	blt	1f
	lwz	%r0, 4(%r5)		/* font->img.format */
	cmpwi	%r0, 9			/* 9 = RGB555 + alpha: a colour font */
	beq	colour_draw_char	/* tail call: it returns straight to our caller */
1:	stwu	%r1, -144(%r1)		/* the instruction we replaced */
	b	font_draw_char_cont	/* 0x42DB4: the rest of the original */
