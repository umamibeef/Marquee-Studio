/* Weather: a template ("format", default "{temp}\xb0 {sky}") filled from the values the Studio's weather feed sends
 * (studio/marquee_studio/feeds/weather.py: temp, feels, hi, lo, wind, sky (an emoji for the sky), sky_text);
 * "--" for values older than "max_age" seconds (default one hour), as when the feed has stopped. */
#include "show.h"

static char tpl[96];
static i32 max_age;

int init(void)
{
	if (setting_str("format", tpl, sizeof tpl) <= 0)
		str_copy(tpl, "{temp}\xb0 {sky}", sizeof tpl);
	max_age = setting_int("max_age", 3600);
	show_init();
	return 0;
}

int frame(unsigned t)
{
	char s[160];
	expand(tpl, s, sizeof s, max_age);
	return show_frame(s, t);
}
