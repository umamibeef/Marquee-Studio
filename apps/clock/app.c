/* Clock: the sign's time in a strftime format ("format", default "%H:%M"); "--:--" until the Studio has set the
 * sign's time. */
#include "show.h"

static char fmt[64];

int init(void)
{
	if (setting_str("format", fmt, sizeof fmt) <= 0)
		str_copy(fmt, "%H:%M", sizeof fmt);
	show_init();
	return 0;
}

int frame(unsigned t)
{
	char s[64];
	i64 now_s = now();
	if (now_s < 0)
		str_copy(s, "--:--", sizeof s);
	else
		fmt_time(s, sizeof s, fmt, now_s);
	return show_frame(s, t);
}
