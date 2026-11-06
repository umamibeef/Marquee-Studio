/* Minimal libc for lwIP in the freestanding bootloader. Simple byte loops: the network window
 * is not performance critical, and these must not depend on anything else. */
#include <stddef.h>
#include <stdint.h>
#include "libc/string.h"
#include "libc/stdlib.h"

void *memcpy(void *d, const void *s, size_t n)
{
	uint8_t *dp = d;
	const uint8_t *sp = s;

	while (n--)
		*dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	uint8_t *dp = d;
	const uint8_t *sp = s;

	if (dp < sp) {
		while (n--)
			*dp++ = *sp++;
	} else {
		while (n--)
			dp[n] = sp[n];
	}
	return d;
}

void *memset(void *d, int c, size_t n)
{
	uint8_t *dp = d;

	while (n--)
		*dp++ = (uint8_t)c;
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (uint8_t)*a - (uint8_t)*b;
		if (!*a)
			break;
	}
	return 0;
}

int strcmp(const char *a, const char *b)
{
	return strncmp(a, b, (size_t)-1);
}

char *strstr(const char *h, const char *n)
{
	size_t len = strlen(n);

	for (; *h; h++)
		if (!strncmp(h, n, len))
			return (char *)h;
	return len ? NULL : (char *)h;
}

int atoi(const char *s)
{
	int v = 0, neg = 0;

	if (*s == '-')
		neg = 1, s++;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return neg ? -v : v;
}

void abort(void)
{
	for (;;)
		;
}
