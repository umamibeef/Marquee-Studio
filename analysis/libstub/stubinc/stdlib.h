#ifndef STUB_STDLIB_H
#define STUB_STDLIB_H
#include <stddef.h>
void *malloc(); void *calloc(); void *realloc(); void free(); void exit(); void abort(); int atoi();
long atol(); double atof(); char *getenv(); int abs(); long labs(); double strtod(); long strtol();
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif
