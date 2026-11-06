/* Counter: n = start + step for every "every" seconds since "since" (sign seconds), shown with "format" where {n}
 * stands for the number (default "{n}"). Blank until the sign's time is set. */
#include "show.h"

static char fmt[64];
static i32 start, step, every;
static i64 since;

int init(void)
{
	char v[24];
	if (setting_str("format", fmt, sizeof fmt) <= 0)
		str_copy(fmt, "{n}", sizeof fmt);
	start = setting_int("start", 0);
	step = setting_int("step", 1);
	every = setting_int("every", 60);
	if (every < 1)
		every = 1;
	since = setting_str("since", v, sizeof v) > 0 ? setting_int("since", 0) : 0;
	show_init();
	return 0;
}

int frame(unsigned t)
{
	char s[96], num[12];
	i64 n_now = now();
	if (n_now < 0)
		return 1;                                      /* no time yet: skip this turn */
	i64 d = n_now - since;
	i64 k = d >= 0 ? d / every : -((-d + every - 1) / every);
	str_int(num, start + step * (i32)k);
	i32 o = 0;
	for (i32 i = 0; fmt[i] && o < 90; i++) {
		if (fmt[i] == '{' && fmt[i + 1] == 'n' && fmt[i + 2] == '}') {
			for (i32 j = 0; num[j] && o < 90; j++)
				s[o++] = num[j];
			i += 2;
		} else
			s[o++] = fmt[i];
	}
	s[o] = 0;
	return show_frame(s, t);
}
