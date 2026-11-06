/* strftime's conversions: strftime.ts, on seconds rather than a Date. Uses no 64-bit division. */
#include "engine.h"

struct out { char *p; uint32_t n, max; };
static void put(struct out *o, char c)
{
	if (o->n + 1 < o->max)
		o->p[o->n++] = c;
}
static void puts_(struct out *o, const char *s)
{
	while (*s)
		put(o, *s++);
}
static void putn(struct out *o, int32_t v, int width, char pad)
{
	char b[12];
	int n = 0, k;
	uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;

	do {
		b[n++] = (char)('0' + u % 10);
		u /= 10;
	} while (u);
	if (v < 0)
		put(o, '-');
	for (k = n; k < width; k++)
		put(o, pad);
	while (n)
		put(o, b[--n]);
}

static const char DAYS[7][10] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char MONTHS[12][10] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December"};

static int32_t fdiv32(int32_t a, int32_t b)
{
	int32_t q = a / b;
	return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* t = days * 86400 + rem, 0 <= rem < 86400, by long division in base 2^16 on the magnitude */
static int32_t days_of(int64_t t, int32_t *rem)
{
	int neg = t < 0;
	uint64_t m = neg ? (uint64_t)0 - (uint64_t)t : (uint64_t)t;
	uint32_t r = 0, q = 0;
	int sh;

	for (sh = 3; sh >= 0; sh--) {
		uint32_t digit = (sh == 3 ? (uint32_t)(m >> 48) : sh == 2 ? (uint32_t)(m >> 32) : sh == 1 ? (uint32_t)(m >> 16) : (uint32_t)m) & 0xFFFF;
		/* r < 86400 < 2^17: r * 65536 fits in 33 bits, so split it */
		uint32_t i;
		for (i = 0; i < 16; i++) {
			r = r << 1 | ((digit >> (15 - i)) & 1);
			q <<= 1;
			if (r >= 86400) {
				r -= 86400;
				q |= 1;
			}
		}
	}
	if (neg && r) {
		*rem = 86400 - (int32_t)r;
		return -(int32_t)q - 1;
	}
	*rem = (int32_t)r;
	return neg ? -(int32_t)q : (int32_t)q;
}

uint32_t sign_strftime(char *out, uint32_t max, const char *fmt, int64_t t)
{
	struct out o = {out, 0, max};
	int32_t rem, days = days_of(t, &rem);
	int32_t hour = rem / 3600, min = rem / 60 % 60, sec = rem % 60;
	int32_t wday = ((days + 4) % 7 + 7) % 7;
	int32_t z = days + 719468, era = fdiv32(z, 146097), doe = z - era * 146097;
	int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int32_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
	int32_t mday = doy - (153 * mp + 2) / 5 + 1, mon = (mp < 10 ? mp + 3 : mp - 9) - 1, year = y + (mon < 2);
	static const uint16_t before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	int32_t yday = before[mon] + mday - 1 + (leap && mon > 1), h12 = hour % 12 ? hour % 12 : 12;
	uint32_t i;

	for (i = 0; fmt[i]; i++) {
		if (fmt[i] != '%' || !fmt[i + 1]) {
			put(&o, fmt[i]);
			continue;
		}
		char c = fmt[++i];
		switch (c) {
		case 'H': putn(&o, hour, 2, '0'); break;
		case 'M': putn(&o, min, 2, '0'); break;
		case 'S': putn(&o, sec, 2, '0'); break;
		case 'I': putn(&o, h12, 2, '0'); break;
		case 'l': putn(&o, h12, 1, '0'); break;
		case 'p': puts_(&o, hour < 12 ? "AM" : "PM"); break;
		case 'P': puts_(&o, hour < 12 ? "am" : "pm"); break;
		case 'k': putn(&o, hour, 2, ' '); break;
		case 'j': putn(&o, yday + 1, 3, '0'); break;
		case 'u': putn(&o, wday ? wday : 7, 1, '0'); break;
		case 'w': putn(&o, wday, 1, '0'); break;
		case 'F': putn(&o, year, 1, '0'); put(&o, '-'); putn(&o, mon + 1, 2, '0'); put(&o, '-'); putn(&o, mday, 2, '0'); break;
		case 'D': putn(&o, mon + 1, 2, '0'); put(&o, '/'); putn(&o, mday, 2, '0'); put(&o, '/'); putn(&o, year % 100, 2, '0'); break;
		case 'a': put(&o, DAYS[wday][0]); put(&o, DAYS[wday][1]); put(&o, DAYS[wday][2]); break;
		case 'A': puts_(&o, DAYS[wday]); break;
		case 'b': put(&o, MONTHS[mon][0]); put(&o, MONTHS[mon][1]); put(&o, MONTHS[mon][2]); break;
		case 'B': puts_(&o, MONTHS[mon]); break;
		case 'd': putn(&o, mday, 2, '0'); break;
		case 'e': putn(&o, mday, 2, ' '); break;
		case 'm': putn(&o, mon + 1, 2, '0'); break;
		case 'y': putn(&o, year % 100, 2, '0'); break;
		case 'Y': putn(&o, year, 1, '0'); break;
		case 'T': putn(&o, hour, 2, '0'); put(&o, ':'); putn(&o, min, 2, '0'); put(&o, ':'); putn(&o, sec, 2, '0'); break;
		case 'R': putn(&o, hour, 2, '0'); put(&o, ':'); putn(&o, min, 2, '0'); break;
		case '%': put(&o, '%'); break;
		default: put(&o, '%'); put(&o, c); break;
		}
	}
	if (max)
		out[o.n] = 0;
	return o.n;
}
