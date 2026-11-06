/* The two hottest hooks of the system simulator (sim/appsim.py), in C. Built at first use by appsim.SimCore
 * (cc -O2 -shared) into build/sim/; it needs no Unicorn headers (the functions come in through struct sim).
 *
 * 1. Instruction counting. emu_start(count=N) makes Unicorn call a code hook before every instruction, which was
 *    most of the simulator's time. fastcount_block runs once per translated block instead: it adds the block's
 *    instructions to `count` and, when the block would take the count past `limit`, stops the emulation before
 *    the block runs (the hook is called before the block's first instruction, and uc_emu_stop takes effect at
 *    once). appsim then runs the remaining instructions of that block on their own, so a slice ends on exactly the
 *    same instruction as with count=N. A hook that moves the PC in the middle of a block calls skip_rest: the rest
 *    of that block does not run and is taken off the count.
 *
 * 2. The MPC8xx special instructions RTEMS uses to disable and enable interrupts (mfmsr, mtmsr, mtspr EIE/EID/NRI),
 *    about one in a hundred instructions, against appsim's shadow MSR (here: msr). Any other instruction word at a
 *    hooked site goes to the Python handler (appsim.AppBoard._special_app), which does the same as before. */
#include <stdint.h>

typedef int (*stop_fn)(void *uc);
typedef int (*reg_read_fn)(void *uc, int regid, void *value);
typedef int (*reg_write_fn)(void *uc, int regid, const void *value);
typedef int (*mem_read_fn)(void *uc, uint64_t address, void *bytes, uint64_t size);
typedef void (*code_fn)(void *uc, uint64_t address, uint32_t size, void *user_data);

struct sim {
    uint64_t count;       /* instructions of the blocks entered */
    uint64_t limit;       /* stop before a block that would take count past this */
    uint64_t block;       /* start of the last block entered */
    uint64_t block_n;     /* its length in instructions */
    uint64_t stop_pc;     /* the block stopped before (valid while stopped != 0) */
    uint64_t stop_n;
    uint64_t stopped;
    uint64_t msr;         /* the shadow MSR (appsim.AppBoard.msr) */
    int64_t reg_pc, reg_0;
    stop_fn emu_stop;
    reg_read_fn reg_read;
    reg_write_fn reg_write;
    mem_read_fn mem_read;
    code_fn python;       /* the Python handler for every other special instruction */
};

#define EE 0x8000
#define RI 0x2

void fastcount_block(void *uc, uint64_t address, uint32_t size, void *user_data)
{
    struct sim *s = user_data;
    uint64_t n = size >> 2;
    if (s->count + n > s->limit) {
        s->stopped = 1;
        s->stop_pc = address;
        s->stop_n = n;
        s->emu_stop(uc);
        return;
    }
    s->count += n;
    s->block = address;
    s->block_n = n;
}

void fastcount_skip_rest(struct sim *s, uint64_t address)
{
    uint64_t k = (address - s->block) / 4 + 1;
    if (0 < k && k < s->block_n) {
        s->count -= s->block_n - k;
        s->block_n = k;
    }
}

void fastcount_special(void *uc, uint64_t address, uint32_t size, void *user_data)
{
    struct sim *s = user_data;
    uint8_t b[4];
    uint64_t v = 0;
    if (s->mem_read(uc, address, b, 4)) {
        s->python(uc, address, size, 0);
        return;
    }
    uint32_t w = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    uint32_t op = w >> 26, xo = (w >> 1) & 0x3FF, rs = (w >> 21) & 0x1F;
    uint32_t spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5);
    if (op == 31 && xo == 83) {                         /* mfmsr */
        v = s->msr;
        s->reg_write(uc, (int)(s->reg_0 + rs), &v);
    } else if (op == 31 && xo == 146) {                 /* mtmsr */
        s->reg_read(uc, (int)(s->reg_0 + rs), &v);
        s->msr = v;
    } else if (op == 31 && xo == 467 && spr == 80) {    /* mtspr EIE */
        s->msr |= EE | RI;
    } else if (op == 31 && xo == 467 && spr == 81) {    /* mtspr EID */
        s->msr = (s->msr & ~(uint64_t)EE) | RI;
    } else if (op == 31 && xo == 467 && spr == 82) {    /* mtspr NRI */
        s->msr &= ~(uint64_t)(EE | RI);
    } else {
        s->python(uc, address, size, 0);
        return;
    }
    v = address + 4;
    s->reg_write(uc, (int)s->reg_pc, &v);
    fastcount_skip_rest(s, address);
}
