/* What the engine needs from the C runtime: nothing else may be used. */
#ifndef ENGINE_RT_H
#define ENGINE_RT_H
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);
void *malloc(__SIZE_TYPE__ n);
void free(void *p);

static inline int32_t eng_s16(int32_t v) { return (int32_t)(int16_t)(uint16_t)(uint32_t)v; }
#endif
