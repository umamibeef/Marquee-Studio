/* drawtest: the helpers the five apps do not use, for appvm_firmware.test.ts: a bundled picture with transparency
 * and two frames (image, asset_find, asset_info), rect with the wipe, pixel, text with a thick outline. */
#include "sign.h"

static i32 pic, frames;

int init(void)
{
	pic = asset_find("arrow");
	frames = asset_info(pic, 2);
	return pic < 0 || frames != 2;
}

int frame(unsigned t)
{
	clear(BLACK);
	rect(0, 12, 200, 4, PAINT_WIPE);
	for (i32 x = 0; x < 200; x += 7)
		pixel(x, (x / 7 + (i32)t) % 12, CYAN);
	for (i32 k = 0; k < 6; k++)
		image(10 + k * 30 + (i32)(t % 30), 2, pic, (i32)(t / 10) + k);
	text(0, 150, 1, MAGENTA, "DRAW");
	outline(OUTLINE_THICK, O_YELLOW);
	return t >= 160;
}
