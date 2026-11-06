/* A Compose message's frame: message.ts render_message555, for text with no native emoji and no in-text commands;
 * amount_format. */
#include "engine.h"
#include "rt.h"

void draw_text_item(struct image *frame, struct image *mask, uint32_t word, struct rect *window, int32_t style,
                    int32_t colour, struct image *blank)
{
	struct rect r = *window, sr = *window, r3 = *window, clip = {0, 0, (int32_t)frame->w, (int32_t)frame->h};
	struct point at = {window->x, window->y};
	int32_t ol[2] = {style, (int32_t)((uint32_t)colour << 16)};

	glyph_blit_effect(blank, frame, word, &r, &at);
	glyph_outline_blit(mask, &sr, frame, &at, ol, &clip);
	glyph_blit_effect(mask, frame, (word & 0xFFFF0000u) | 0x8000u, &r3, &at);
}

static uint32_t gcd_(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;
		a = b, b = t;
	}
	return a;
}

uint32_t message_frame_count(const struct message_params *m)
{
	struct sbuf t = {m->text, (uint16_t)m->len, (uint16_t)m->len};
	uint32_t tw = (uint32_t)text_width(m->f, &t);

	if (!m->is_static) {
		uint32_t rate = m->text_rate > 1 ? (uint32_t)m->text_rate : 1;
		return (tw + m->W + rate - 1) / rate;                /* shift = 0, rate, ... while shift < tw + W */
	}
	uint32_t periods[4], np = 0, n = 1, big = 1, i, c, k;

	for (k = 0; k < 2; k++) {
		c = k ? m->word & 0xFFFF : m->word >> 16;
		if (c == 0x8001)
			periods[np++] = 12 * m->H;
		else if (c == 0x8002)
			periods[np++] = 24;
		else if (c == 0xBFFE || c == 0xBFFF)
			periods[np++] = 48;
		else if (c >= 0xC000 && (c & 0x3FFF) < 100)
			periods[np++] = effect_slot_period(c & 0x3FFF);
	}
	if (m->flash_on > 0)
		periods[np++] = (uint32_t)m->flash_on + (m->flash_off > 1 ? (uint32_t)m->flash_off : 1);
	for (i = 0; i < np; i++) {
		if (n <= 0x7FFFFFFFu / periods[i])
			n = n * periods[i] / gcd_(n, periods[i]);
		else
			n = 0xFFFFFFFFu;
		big = periods[i] > big ? periods[i] : big;
	}
	return n <= 480 ? n : big;
}

void message_frame(const struct message_params *m, uint32_t k, struct image *frame, struct image *mask,
                   struct image *blank)
{
	struct sbuf t = {m->text, (uint16_t)m->len, (uint16_t)m->len};
	int32_t W = (int32_t)m->W, H = (int32_t)m->H, ty = (H - (int32_t)font_cell_h(m->f)) / 2, x;
	int flashed = m->flash_on <= 0 ||
	              k % ((uint32_t)m->flash_on + (m->flash_off > 1 ? (uint32_t)m->flash_off : 1)) < (uint32_t)m->flash_on;
	uint16_t colours[2] = {0x7FFF, 0}, wo;
	struct rect window = {0, 0, W, H};

	if (m->is_static)
		x = (W - text_width(m->f, &t)) / 2;
	else
		x = W - (int32_t)k * (m->text_rate > 1 ? m->text_rate : 1);
	effect_frame_count = m->frame0 + k;
	memset(frame->pixels, 0, m->W * m->H * 2);
	memset(mask->pixels, 0, m->W * m->H * 2);
	if (flashed) {
		struct point at = {x, ty};
		text_draw_proportional(mask, &at, m->f, &t, colours, &wo);
	}
	draw_text_item(frame, mask, m->word, &window, m->outline_style, m->outline_colour, blank);
}

uint32_t amount_format(uint8_t *out, uint32_t max, uint32_t value, uint32_t decimals, uint32_t mode, uint8_t dec,
                       uint8_t thou, const uint8_t *prefix, uint32_t prefix_len, const uint8_t *suffix,
                       uint32_t suffix_len)
{
	uint8_t d[64];                                           /* the digits, with their marks */
	uint32_t n = 0, v = value, i, o = 0;

	do {
		d[n++] = (uint8_t)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n < decimals + 1 && n < 20)
		d[n++] = '0';
	for (i = 0; i < n / 2; i++) {
		uint8_t t = d[i];
		d[i] = d[n - 1 - i], d[n - 1 - i] = t;
	}
	if (decimals > n - 1)
		decimals = n - 1;
	if (mode == 2 || mode == 3)
		for (i = n - decimals; i < n; i++)
			d[i] = (uint8_t)(d[i] - 0x30);
	if (mode == 0 || mode == 2)
		for (i = 0; i < n - decimals; i++)
			d[i] = (uint8_t)(d[i] - 0x20);
	if (mode == 0)
		for (i = n - decimals; i < n; i++)
			d[i] = (uint8_t)(d[i] - 0x20);
	uint32_t k = 0, ln = n;
	if (decimals) {
		for (i = ln; i > ln - decimals; i--)
			d[i] = d[i - 1];
		d[ln - decimals] = dec;
		k = decimals + 1;
		ln++;
	}
	if (thou) {
		uint32_t u = k + 3;
		while (u < ln && ln < sizeof d) {
			for (i = ln; i > ln - u; i--)
				d[i] = d[i - 1];
			d[ln - u] = thou;
			u += 4;
			ln++;
		}
	}
	int dollar = (mode == 0 || mode == 2) && prefix_len == 1 && prefix[0] == 0x24;
	for (i = 0; i < prefix_len && o < max; i++)
		out[o++] = dollar ? 0x1C : prefix[i];
	for (i = 0; i < ln && o < max; i++)
		out[o++] = d[i];
	for (i = 0; i < suffix_len && o < max; i++)
		out[o++] = suffix[i];
	return o;
}
