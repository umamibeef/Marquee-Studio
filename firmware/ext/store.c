/* The content store: files uploaded over the DB9 console (apps app0..15.mqap, and later fonts and animations) kept in
 * the main flash's archive volume (volume 1, the main flash at
 * 0x50000000).
 *
 * The main flash's archive chain may hold files of its own (the stock firmware's downloads): ours start on a 64 KB
 * sector boundary after them, behind a marker entry "mq.store" (and a filler entry "mq.pad" up to the boundary the
 * first time), so rewriting the store erases only its own sectors. A commit writes the whole store: the files staged
 * since "sbegin" (sfile / sd per file), then reads it back.
 *
 * The firmware itself only reads the archive copy it makes in RAM at boot (volume 2), so the store is read through
 * store_find, never lfs_open.
 *
 * Main flash: AMD AM29LV128-class parts in word mode, 16 MB per chip select; the erase / program polling is the boot
 * loader's (bootloader/src/mainflash.c). Interrupts are off during each erase and program (nothing may read the
 * main flash meanwhile); the watchdog is serviced while waiting. Entries are the archive's own format
 * (bootloader/tools/mkimage.py make_entry): total length, data length, flags 0, name, data, CRC-32 check word. */
#include <stdint.h>

void *malloc(unsigned long size);
void free(void *p);
unsigned long crc32(unsigned long crc, const uint8_t *buf, unsigned len);
void **apps_store_state(void);
void apps_store_busy(int busy);
void apps_unload(void);
void apps_apply_lines(void);
extern uint32_t lfs_vol_base[3], lfs_vol_size[3];

#define SECTOR   0x10000u
#define MAXFILES 40
#define MF16(a)  (*(volatile uint16_t *)(a))
#define SWSR     (*(volatile uint16_t *)0xFF00000Eu)

struct sfile { char name[40]; uint8_t *data; uint32_t size, got; };
struct store { uint32_t n; struct sfile f[MAXFILES]; };
#define ST ((struct store *)*apps_store_state())

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void store_begin(void)
{
	struct store *s = ST;
	if (s)
		for (uint32_t k = 0; k < s->n; k++)
			free(s->f[k].data);
	if (!s && !(s = malloc(sizeof *s)))
		return;
	s->n = 0;
	*apps_store_state() = s;
}

const char *store_file(const char *name, uint32_t size)
{
	struct store *s = ST;
	if (!s)
		return "no sbegin";
	if (s->n >= MAXFILES)
		return "too many files";
	struct sfile *f = &s->f[s->n];
	uint32_t k = 0;
	while (name[k] && k < sizeof f->name - 1)
		f->name[k] = name[k], k++;
	f->name[k] = 0;
	if (!k || name[k])
		return "bad name";
	if (!(f->data = malloc(size ? size : 1)))
		return "out of memory";
	f->size = size, f->got = 0;
	s->n++;
	return 0;
}

const char *store_data(const uint8_t *p, uint32_t n)
{
	struct store *s = ST;
	if (!s || !s->n)
		return "no sfile";
	struct sfile *f = &s->f[s->n - 1];
	if (f->got + n > f->size)
		return "more data than the file's size";
	for (uint32_t k = 0; k < n; k++)
		f->data[f->got + k] = p[k];
	f->got += n;
	return 0;
}

/* ---- the main flash ---- */
static uint32_t msr_off(void)
{
	uint32_t m;
	__asm__ volatile("mfmsr %0" : "=r"(m));
	__asm__ volatile("mtmsr %0; isync" : : "r"(m & ~0x8000u));      /* EE off */
	return m;
}
static void msr_on(uint32_t m) { __asm__ volatile("mtmsr %0; isync" : : "r"(m)); }
static void kick(void) { SWSR = 0x556C; SWSR = 0xAA39; }

static uint32_t chip_of(uint32_t a) { return a & ~0xFFFFFFu; }
static void unlock(uint32_t chip) { MF16(chip + 0xAAA) = 0xAA; MF16(chip + 0x554) = 0x55; }

static int wait_done(uint32_t a, uint16_t v, int erase)
{
	for (uint32_t n = 0; n < 40000000u; n++) {
		uint16_t s = MF16(a);
		if (erase ? s == 0xFFFF : (s & 0x80) == (v & 0x80))
			return MF16(a) == v ? 0 : -1;
		if ((n & 0xFFF) == 0)
			kick();
	}
	return -1;
}

int store_erase_sector(uint32_t a);
int store_program(uint32_t a, const uint8_t *p, uint32_t n);
static int erase_sector(uint32_t a)
{
	uint32_t chip = chip_of(a), m = msr_off();
	unlock(chip);
	MF16(chip + 0xAAA) = 0x80;
	unlock(chip);
	MF16(a) = 0x30;
	int r = wait_done(a, 0xFFFF, 1);
	MF16(chip) = 0xF0;
	msr_on(m);
	return r;
}

static int program_bytes(uint32_t a, const uint8_t *p, uint32_t n)
{
	for (uint32_t k = 0; k < n; k += 2) {
		uint16_t v = (uint16_t)(p[k] << 8 | (k + 1 < n ? p[k + 1] : 0xFF));
		uint32_t chip = chip_of(a + k), m = msr_off();
		unlock(chip);
		MF16(chip + 0xAAA) = 0xA0;
		MF16(a + k) = v;
		int r = wait_done(a + k, v, 0);
		msr_on(m);
		if (r) {
			MF16(chip) = 0xF0;
			return -1;
		}
		if ((k & 0x3FF) == 0)
			kick();
	}
	return 0;
}

/* the update slot's writes (update.c) */
int store_erase_sector(uint32_t a) { return erase_sector(a); }
int store_program(uint32_t a, const uint8_t *p, uint32_t n) { return program_bytes(a, p, n); }

/* one archive entry, as mkimage.py's make_entry makes it */
static uint32_t entry_len(uint32_t namelen, uint32_t size) { return ((namelen + 16) & ~3u) + ((size + 3) & ~3u) + 4; }

static uint8_t *entry(const char *name, const uint8_t *data, uint32_t size, uint32_t *total)
{
	uint32_t nl = 0;
	while (name[nl])
		nl++;
	uint32_t hl = (nl + 16) & ~3u, t = entry_len(nl, size);
	uint8_t *e = malloc(t);
	if (!e)
		return 0;
	for (uint32_t k = 0; k < t; k++)
		e[k] = 0;
	e[0] = (uint8_t)(t >> 24), e[1] = (uint8_t)(t >> 16), e[2] = (uint8_t)(t >> 8), e[3] = (uint8_t)t;
	e[4] = (uint8_t)(size >> 24), e[5] = (uint8_t)(size >> 16), e[6] = (uint8_t)(size >> 8), e[7] = (uint8_t)size;
	e[12] = (uint8_t)nl;
	for (uint32_t k = 0; k < nl; k++)
		e[13 + k] = (uint8_t)name[k];
	for (uint32_t k = 0; k < size; k++)
		e[hl + k] = data ? data[k] : 0;
	uint32_t c = crc32(crc32(0, 0, 0), e, t - 4);
	e[t - 4] = (uint8_t)(c >> 24), e[t - 3] = (uint8_t)(c >> 16), e[t - 2] = (uint8_t)(c >> 8), e[t - 1] = (uint8_t)c;
	*total = t;
	return e;
}

static int name_is(uint32_t a, const char *name)
{
	const uint8_t *p = (const uint8_t *)a;
	uint32_t nl = p[12], k = 0;
	while (name[k] && k < nl && p[13 + k] == (uint8_t)name[k])
		k++;
	return k == nl && !name[k];
}

/* walk the volume-1 chain: *ours = our marker's address (0 if none), returns the chain's end */
static uint32_t chain(uint32_t base, uint32_t size, uint32_t *ours)
{
	uint32_t a = base;
	*ours = 0;
	while (a + 16 <= base + size) {
		uint32_t n = be32((const uint8_t *)a);
		if (n == 0 || n == 0xFFFFFFFFu || a + n > base + size || n < 16)
			break;
		if (!*ours && name_is(a, "mq.store"))
			*ours = a;
		a = (a + n + 3) & ~3u;
	}
	return a;
}

const char *store_commit(void)
{
	struct store *s = ST;
	uint32_t base = lfs_vol_base[1], size = lfs_vol_size[1];
	if (!s)
		return "no sbegin";
	if (!base || size < 4 * SECTOR)
		return "no main flash";
	for (uint32_t k = 0; k < s->n; k++)
		if (s->f[k].got != s->f[k].size)
			return "a file is incomplete";
	uint32_t ours, end = chain(base, size, &ours), start, old_end = end;
	if (!ours) {                                          /* after the foreign chain, on a sector boundary */
		start = (end + SECTOR - 1) & ~(SECTOR - 1);
		if (start != end && start - end < 24)
			start += SECTOR;
	} else
		start = ours;
	uint32_t need = entry_len(8, 4);
	for (uint32_t k = 0; k < s->n; k++) {
		uint32_t nl = 0;
		while (s->f[k].name[nl])
			nl++;
		need += entry_len(nl, s->f[k].size);
	}
	if (start + need + 4 > base + size)
		return "the store does not fit";
	apps_store_busy(1);
	apps_unload();
	const char *err = 0;
	uint32_t top = (old_end > start + need ? old_end : start + need) + 4;
	for (uint32_t a = start; a < top && !err; a += SECTOR)
		if (erase_sector(a))
			err = "erase failed";
	if (!ours && start != end && !err) {                  /* a filler from the foreign chain's end to ours */
		uint32_t t, gap = start - end;
		uint8_t *pad = entry("mq.pad", 0, gap - entry_len(6, 0), &t);
		if (!pad || t != gap || program_bytes(end, pad, t))
			err = "filler entry failed";
		free(pad);
	}
	uint32_t a = start;
	const uint8_t ver[4] = {0, 0, 0, 1};
	for (int32_t k = -1; k < (int32_t)s->n && !err; k++) {
		uint32_t t;
		uint8_t *e = k < 0 ? entry("mq.store", ver, 4, &t) : entry(s->f[k].name, s->f[k].data, s->f[k].size, &t);
		if (!e) {
			err = "out of memory";
			break;
		}
		if (program_bytes(a, e, t))
			err = "program failed";
		else
			for (uint32_t i = 0; i < t; i++)
				if (((const uint8_t *)a)[i] != e[i]) {
					err = "read-back differs";
					break;
				}
		free(e);
		a = (a + t + 3) & ~3u;
	}
	apps_store_busy(0);
	if (!err)
		apps_apply_lines();
	return err;
}

/* the files in the store: name, size; returns the count */
uint32_t store_list(void (*each)(const char *name, uint32_t nl, uint32_t size))
{
	uint32_t base = lfs_vol_base[1], size = lfs_vol_size[1], ours, n = 0;
	if (!base)
		return 0;
	uint32_t end = chain(base, size, &ours);
	for (uint32_t a = ours; a && a < end;) {
		const uint8_t *p = (const uint8_t *)a;
		uint32_t t = be32(p);
		if (!name_is(a, "mq.store")) {
			each((const char *)p + 13, p[12], be32(p + 4));
			n++;
		}
		a = (a + t + 3) & ~3u;
	}
	return n;
}

/* a file of the store by name: its data and size (main flash stays mapped though the firmware disables archive
 * volume 1 once its content is copied to RAM, so lfs_open does not see the store) */
int store_find(const char *name, const uint8_t **data, uint32_t *size)
{
	uint32_t base = lfs_vol_base[1], vsize = lfs_vol_size[1], ours;
	if (!base)
		return 0;
	uint32_t end = chain(base, vsize, &ours);
	for (uint32_t a = ours; a && a < end;) {
		const uint8_t *p = (const uint8_t *)a;
		uint32_t t = be32(p);
		if (name_is(a, name)) {
			*data = p + ((p[12] + 16u) & ~3u);
			*size = be32(p + 4);
			return 1;
		}
		a = (a + t + 3) & ~3u;
	}
	return 0;
}
