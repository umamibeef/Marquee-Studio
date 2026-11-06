/* Freestanding shim so third-party C compiled from source finds the few libc calls we provide (rt.c). */
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
void *malloc(size_t);
void *calloc(size_t, size_t);
void free(void *);
