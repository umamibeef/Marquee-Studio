/* Rotating text: the "messages" (one per line) in turn, a new one every "every" seconds (by the sign's clock, or
 * its uptime before the time is set). */
#include "show.h"

static char msgs[256];
static i32 every, count;

int init(void)
{
	if (setting_str("messages", msgs, sizeof msgs) < 0)
		msgs[0] = 0;
	every = setting_int("every", 10);
	if (every < 1)
		every = 1;
	count = 0;
	for (i32 i = 0; msgs[i]; i++)
		if (msgs[i] != '\n' && (i == 0 || msgs[i - 1] == '\n'))
			count++;
	show_init();
	return 0;
}

static i64 turn_start;

int frame(unsigned t)
{
	char s[256];
	if (!count)
		return 1;
	if (t == 0)
		turn_start = now();
	i64 secs = turn_start < 0 ? 0 : turn_start;
	i32 want = (i32)((secs / every) % count), k = 0, o = 0;
	for (i32 i = 0; msgs[i]; i++)
		if (msgs[i] != '\n' && (i == 0 || msgs[i - 1] == '\n')) {
			if (k++ == want) {
				while (msgs[i] && msgs[i] != '\n' && o < 255)
					s[o++] = msgs[i++];
				break;
			}
		}
	s[o] = 0;
	return show_frame(s, t);
}
