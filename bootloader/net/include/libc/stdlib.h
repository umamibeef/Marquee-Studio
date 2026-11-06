#ifndef BOOT_STDLIB_H
#define BOOT_STDLIB_H
#include <stddef.h>
int atoi(const char *s);
void abort(void) __attribute__((noreturn));
#endif
