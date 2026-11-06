/* Tiny freestanding runtime for the from-source app: memset/memcpy and the 64-bit divide/modulo helpers the eBPF
 * interpreter's DIV/MOD opcodes need (PowerPC has no 64-bit divide instruction and we link no libgcc). From source. */
#include <stdint.h>
#include <stddef.h>

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

static uint64_t udivmod(uint64_t num, uint64_t den, uint64_t *rem)
{
	uint64_t q = 0, r = 0;
	if (!den) { if (rem) *rem = num; return ~(uint64_t)0; }
	for (int i = 63; i >= 0; i--) {
		r = (r << 1) | ((num >> i) & 1u);
		if (r >= den) { r -= den; q |= (uint64_t)1 << i; }
	}
	if (rem) *rem = r;
	return q;
}
uint64_t __udivdi3(uint64_t a, uint64_t b) { return b ? udivmod(a, b, 0) : 0; }
uint64_t __umoddi3(uint64_t a, uint64_t b) { uint64_t r; if (!b) return 0; udivmod(a, b, &r); return r; }
int64_t __divdi3(int64_t a, int64_t b)
{
	int neg = 0; uint64_t ua = a < 0 ? (neg ^= 1, (uint64_t)(-a)) : (uint64_t)a, ub = b < 0 ? (neg ^= 1, (uint64_t)(-b)) : (uint64_t)b;
	uint64_t q = ub ? udivmod(ua, ub, 0) : 0; return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t a, int64_t b)
{
	int neg = a < 0; uint64_t ua = a < 0 ? (uint64_t)(-a) : (uint64_t)a, ub = b < 0 ? (uint64_t)(-b) : (uint64_t)b;
	uint64_t r; if (!ub) return 0; udivmod(ua, ub, &r); return neg ? -(int64_t)r : (int64_t)r;
}

/* ---- the rest of the small libc: strings and the heap ---- */
int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	for (; n--; p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

/* First-fit heap over the RAM between the end of the image and the top of RAM (hand-off word 0x14). Each block has a
 * 16-byte header so payloads stay on a cache line; free blocks are kept in address order and merged with neighbours. */
extern char __heap_start[];
struct blk { struct blk *next; size_t size; size_t pad[2]; };   /* size includes the header */
static struct blk *g_free;
static int g_heap_ready;
#define HDR ((size_t)sizeof(struct blk))

static void heap_init(void)
{
	uintptr_t a = ((uintptr_t)__heap_start + 15u) & ~(uintptr_t)15u;
	uintptr_t top = *(volatile uint32_t *)0x14u & ~(uint32_t)15u;
	g_heap_ready = 1;
	if (top <= a + HDR)
		return;
	g_free = (struct blk *)a;
	g_free->next = 0;
	g_free->size = top - a;
}

void *malloc(size_t n)
{
	if (!g_heap_ready)
		heap_init();
	if (n > 0xFFFF0000u)
		return 0;
	size_t need = ((n + 15u) & ~(size_t)15u) + HDR;
	for (struct blk **pp = &g_free; *pp; pp = &(*pp)->next) {
		struct blk *b = *pp;
		if (b->size < need)
			continue;
		if (b->size >= need + 2 * HDR) {
			struct blk *r = (struct blk *)((char *)b + need);
			r->next = b->next;
			r->size = b->size - need;
			*pp = r;
			b->size = need;
		} else
			*pp = b->next;
		return (char *)b + HDR;
	}
	return 0;
}

void free(void *p)
{
	if (!p)
		return;
	struct blk *b = (struct blk *)((char *)p - HDR), **pp = &g_free;
	while (*pp && *pp < b)
		pp = &(*pp)->next;
	b->next = *pp;
	*pp = b;
	if (b->next && (char *)b + b->size == (char *)b->next) {
		b->size += b->next->size;
		b->next = b->next->next;
	}
	struct blk *prev = 0;
	for (struct blk *q = g_free; q && q != b; q = q->next)
		prev = q;
	if (prev && (char *)prev + prev->size == (char *)b) {
		prev->size += b->size;
		prev->next = b->next;
	}
}

void *calloc(size_t a, size_t b)
{
	size_t n = a * b;
	if (a && n / a != b)
		return 0;
	void *p = malloc(n);
	if (p)
		memset(p, 0, n);
	return p;
}
