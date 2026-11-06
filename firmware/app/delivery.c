/* Framed delivery on top of the apps: UMFRAME frames on the DB9 (while its console is closed) and RS-485 set the clock,
 * push data values, carry USC1 schedules and firmware updates, with an answer on the line the frame came from.
 *
 * A USC1 schedule (our declarative format) becomes attract lines: each entry's text, as message markup, on its line,
 * with its fill, background and outline in the line's effect words. {key} in a text is replaced by the data value of
 * that name each time the line starts. Data values also go to the apps that ask for them (apps_data). */
#include <stdint.h>
#include "core.h"
#include "hw.h"
#include "serial.h"
#include "serial2.h"
#include "umframe.h"
#include "delivery.h"

int lfs_open_bytes(const char *name, const uint8_t **data, uint32_t *size);
void apps_set_time(uint32_t secs);
int apps_data(unsigned line, const char *key, const uint8_t *value, unsigned n);
const char *update_begin(uint32_t size, uint32_t crc);
int update_data(const uint8_t *p, uint32_t n);
const char *update_commit(void);

#define UMF_ADDR 0x01u          /* this sign's address */
#define UMF_ALL  0xFFu

/* ---- data values ---- */
#define NFEED 16
static struct feedkv { char key[16]; char val[48]; } g_feed[NFEED];
static int g_nfeed;

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static void feed_set(const char *key, const char *val)
{
	int i;
	for (i = 0; i < g_nfeed; i++)
		if (str_eq(g_feed[i].key, key))
			break;
	if (i == g_nfeed) {
		if (g_nfeed == NFEED)
			return;
		g_nfeed++;
		int k = 0;
		for (; key[k] && k < 15; k++)
			g_feed[i].key[k] = key[k];
		g_feed[i].key[k] = 0;
	}
	int v = 0;
	for (; val[v] && v < 47; v++)
		g_feed[i].val[v] = val[v];
	g_feed[i].val[v] = 0;
	for (unsigned l = 0; l < NLINES_ATTRACT; l++)
		if (g_attract.mode[l] && !g_attract.text_set[l])
			apps_data(l, key, (const uint8_t *)val, (unsigned)v);
}

/* ---- USC1: the schedule ---- */
static char g_tmpl[NLINES_ATTRACT][128];
static uint16_t g_tmpl_len[NLINES_ATTRACT];
static uint8_t g_prefix[NLINES_ATTRACT][32];   /* the markup before the text */
static uint8_t g_prefix_len[NLINES_ATTRACT];

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

/* the entry in markup: the font, then either a hold (dwell in tenths, the hold's unit is 4 frames at 40 per second) or a
 * scroll through; motion and flash as in-text commands (native_emoji.c) */
static void line_markup(unsigned ln, uint8_t mode, uint32_t dwell, uint8_t font, uint8_t motion, uint32_t flon, uint32_t floff)
{
	uint8_t *m = g_prefix[ln];
	unsigned n = 0;
	if (font != 0xFF)
		m[n++] = 0x1B, m[n++] = 0x60, m[n++] = font;
	else
		m[n++] = 0x1B, m[n++] = 0x60, m[n++] = 0xFF;            /* the sign picks the font and centres */
	if (mode & 1) {
		m[n++] = 0x1B, m[n++] = 0x40, m[n++] = 40;              /* in from the right, 40 px/s */
		m[n++] = 0x1B, m[n++] = 0x50, m[n++] = 0, m[n++] = 40;  /* no hold: right through */
	} else {
		uint32_t hold = dwell ? dwell : 30;                     /* 3 s by default; 4 frames = 1 tenth */
		if (hold > 255)
			hold = 255;
		m[n++] = 0x1B, m[n++] = 0x42, m[n++] = 255;             /* up from the bottom edge at full speed */
		m[n++] = 0x1B, m[n++] = 0x52, m[n++] = (uint8_t)hold, m[n++] = 255;   /* the hold, then away upward */
	}
	if (motion != 0xFF)
		m[n++] = 0x1F, m[n++] = 'm', m[n++] = (uint8_t)('0' + motion);
	if (flon != 0xFFFF && flon) {
		static const char H[] = "0123456789abcdef";
		m[n++] = 0x1F, m[n++] = 'o';
		m[n++] = (uint8_t)H[flon >> 4 & 15], m[n++] = (uint8_t)H[flon & 15];
		m[n++] = (uint8_t)H[floff >> 4 & 15], m[n++] = (uint8_t)H[floff & 15];
	}
	g_prefix_len[ln] = (uint8_t)n;
}

void delivery_refresh_line(unsigned ln)
{
	if (ln >= NLINES_ATTRACT || !g_attract.text_set[ln])
		return;
	uint8_t *o = &g_message_table[ln * 256];
	unsigned n = 0, len = g_tmpl_len[ln];
	const char *t = g_tmpl[ln];
	for (unsigned k = 0; k < g_prefix_len[ln]; k++)
		o[n++] = g_prefix[ln][k];
	for (unsigned i = 0; i < len && n < 254;) {
		if (t[i] == '{') {
			unsigned e = i + 1;
			while (e < len && t[e] != '}')
				e++;
			if (e < len && e - i - 1 < 16) {
				char key[16];
				unsigned kl = e - i - 1;
				for (unsigned k = 0; k < kl; k++)
					key[k] = t[i + 1 + k];
				key[kl] = 0;
				for (int f = 0; f < g_nfeed; f++)
					if (str_eq(g_feed[f].key, key))
						for (const char *v = g_feed[f].val; *v && n < 254; v++)
							o[n++] = (uint8_t)*v;
				i = e + 1;
				continue;
			}
		}
		o[n++] = (uint8_t)t[i++];
	}
	o[n] = 0;
}

static void sched_parse(const uint8_t *p, uint32_t n)
{
	if (n < 16 || p[0] != 'U' || p[1] != 'S' || p[2] != 'C' || p[3] != '1')
		return;
	uint32_t napps = be16(p + 8), nfeeds = be16(p + 10), o = 16;
	for (unsigned l = 0; l < NLINES_ATTRACT; l++)        /* a schedule replaces the text lines before it */
		if (g_attract.text_set[l]) {
			g_attract.text_set[l] = 0;
			g_attract.mode[l] = 0;
			g_message_table[l * 256] = 0;
		}
	for (uint32_t ai = 0; ai < napps; ai++) {
		if (o + 16 > n)
			return;
		uint8_t mode = p[o], font = p[o + 3], motion = p[o + 6], ostyle = p[o + 7], ocolour = p[o + 8], bg = p[o + 13];
		uint32_t dwell = be16(p + o + 1), fill = be16(p + o + 4);
		uint32_t flon = be16(p + o + 9), floff = be16(p + o + 11), tlen = be16(p + o + 14);
		o += 16;
		if (o + tlen > n)
			return;
		if (ai < NLINES_ATTRACT) {
			unsigned k = 0;
			for (; k < tlen && k < sizeof g_tmpl[ai] - 1; k++)
				g_tmpl[ai][k] = (char)p[o + k];
			g_tmpl[ai][k] = 0;
			g_tmpl_len[ai] = (uint16_t)k;
			g_attract.text_set[ai] = 1;
			g_attract.mode[ai] = 1;
			g_attract.effect[ai] = (fill == 0xFFFF ? 0x7FE0u : fill) << 16 | (bg != 0xFF ? 0xC000u | bg : 0u);
			g_attract.outline[ai][0] = ostyle != 0xFF ? ostyle : 0;
			g_attract.outline[ai][1] = (uint32_t)(ocolour & 7) << 16;
			line_markup((unsigned)ai, mode, dwell, font, motion, flon, floff);
		}
		o += tlen;
	}
	for (uint32_t fi = 0; fi < nfeeds && o < n; fi++) {
		char key[16], val[48];
		int kl = 0, vl = 0;
		while (o < n && p[o] && kl < 15)
			key[kl++] = (char)p[o++];
		key[kl] = 0;
		if (o < n && !p[o])
			o++;
		while (o < n && p[o] && vl < 47)
			val[vl++] = (char)p[o++];
		val[vl] = 0;
		if (o < n && !p[o])
			o++;
		feed_set(key, val);
	}
	for (unsigned l = 0; l < NLINES_ATTRACT; l++)
		delivery_refresh_line(l);
}

void delivery_boot(void)
{
	const uint8_t *d;
	uint32_t n;
	if (lfs_open_bytes("sched", &d, &n))
		sched_parse(d, n);
}

/* ---- frames ---- */
static struct rxstate { uint8_t frame[600]; int len, in; } g_rx[2];
static uint8_t g_sched[1024];
static uint32_t g_schedlen;
static int g_schedopen;

static void reply(int chan, uint8_t type, uint16_t seq, const uint8_t *pl, int len)
{
	uint8_t wire[UMF_DECMAX + 32];
	int w = umf_build(0x00, UMF_ADDR, type, seq, pl, len, wire, (int)sizeof wire);
	if (w <= 0)
		return;
	if (chan)
		scc_write((const char *)wire, w);
	else
		serial_write((const char *)wire, w);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void dispatch(int chan, const uint8_t *body, int n)
{
	static uint8_t dec[UMF_DECMAX];
	struct umf f;
	if (umf_parse(body, n, &f, dec, (int)sizeof dec) < 0)
		return;                                          /* bad CRC or length: dropped */
	if (f.dest != UMF_ADDR && f.dest != UMF_ALL)
		return;                                          /* another sign's on the shared bus */
	int ans = f.dest == UMF_ADDR;                        /* a broadcast is not answered: they would collide */
	uint8_t t = UMF_ACKF;
	switch (f.type) {
	case UMF_PING:
		t = UMF_PONG;
		break;
	case UMF_SET_TIME:
		if (f.len >= 4)
			apps_set_time(be32(f.payload));
		break;
	case UMF_FEED: {
		char key[16], val[48];
		int o = 0, k = 0, v = 0;
		while (o < f.len && f.payload[o] && k < 15)
			key[k++] = (char)f.payload[o++];
		key[k] = 0;
		if (o < f.len && !f.payload[o])
			o++;
		while (o < f.len && f.payload[o] && v < 47)
			val[v++] = (char)f.payload[o++];
		val[v] = 0;
		feed_set(key, val);
		break;
	}
	case UMF_SCHED_BEGIN:
		g_schedlen = 0;
		g_schedopen = 1;
		break;
	case UMF_SCHED_CHUNK:
		for (int i = 0; i < f.len && g_schedlen < sizeof g_sched; i++)
			g_sched[g_schedlen++] = f.payload[i];
		break;
	case UMF_SCHED_COMMIT:
		if (g_schedopen) {
			sched_parse(g_sched, g_schedlen);
			g_schedopen = 0;
		}
		break;
	case UMF_UPDATE_BEGIN:
		if (f.len < 8 || update_begin(be32(f.payload), be32(f.payload + 4)))
			t = UMF_NAK;
		break;
	case UMF_UPDATE_CHUNK:
		if (update_data(f.payload, (uint32_t)f.len))
			t = UMF_NAK;
		break;
	case UMF_UPDATE_COMMIT:
		if (update_commit())
			t = UMF_NAK;
		else {
			reply(chan, UMF_ACKF, f.seq, 0, 0);
			delay_us(50000);
			hw_reset();
		}
		break;
	default:
		t = UMF_NAK;
		break;
	}
	if (ans)
		reply(chan, t, f.seq, 0, 0);
}

void delivery_rx(int chan, uint8_t c)
{
	struct rxstate *r = &g_rx[chan];
	if (r->in) {
		if (c == 0x00) {
			dispatch(chan, r->frame, r->len);
			r->in = 0;
			r->len = 0;
		} else if (r->len < (int)sizeof r->frame)
			r->frame[r->len++] = c;
	} else if (c == UMF_SOF) {
		r->in = 1;
		r->len = 0;
	}
}
