/* Countdown to "target" (sign seconds): "format" with {d} {h} {m} {s} (days, hours, minutes, seconds left), then
 * "done" once it is reached. Blank until the sign's time is set. */
#include "show.h"

static char fmt[64], done_text[64];
static i64 target;

int init(void)
{
	if (setting_str("format", fmt, sizeof fmt) <= 0)
		str_copy(fmt, "{d}d {h}h {m}m", sizeof fmt);
	if (setting_str("done", done_text, sizeof done_text) < 0)
		done_text[0] = 0;
	target = setting_int("target", 0);
	show_init();
	return 0;
}

int frame(unsigned t)
{
	char s[96];
	i64 n_now = now();
	if (n_now < 0)
		return 1;
	i64 left = target - n_now;
	if (left <= 0)
		return show_frame(done_text, t);
	i32 v[4] = {(i32)(left / 86400), (i32)(left / 3600 % 24), (i32)(left / 60 % 60), (i32)(left % 60)};
	const char *keys = "dhms";
	i32 o = 0;
	for (i32 i = 0; fmt[i] && o < 90; i++) {
		i32 hit = -1;
		if (fmt[i] == '{' && fmt[i + 1] && fmt[i + 2] == '}')
			for (i32 k = 0; k < 4; k++)
				if (keys[k] == fmt[i + 1])
					hit = k;
		if (hit >= 0) {
			char num[12];
			str_int(num, v[hit]);
			for (i32 j = 0; num[j] && o < 90; j++)
				s[o++] = num[j];
			i += 2;
		} else
			s[o++] = fmt[i];
	}
	s[o] = 0;
	return show_frame(s, t);
}
