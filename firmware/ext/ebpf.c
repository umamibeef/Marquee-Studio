/* The eBPF interpreter (ebpf.h, docs/APPS.md). RFC 9669: ALU and ALU64 (with sdiv/smod and movsx), byte swaps,
 * 64-bit immediate loads, loads (also sign-extending), stores, atomics (add/or/and/xor with fetch, xchg, cmpxchg),
 * JMP and JMP32 conditions, ja and ja32 (gotol), helper calls (also by register, callx), local calls and exit.
 * Division by zero gives 0 and modulo by zero leaves the dividend, as the RFC defines. Memory is little-endian
 * whatever the host's byte order: every access goes byte by byte through the region table. */
#include "ebpf.h"

#define CLS(op)  ((op) & 7)
#define C_LD 0
#define C_LDX 1
#define C_ST 2
#define C_STX 3
#define C_ALU 4
#define C_JMP 5
#define C_JMP32 6
#define C_ALU64 7

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct inst { uint8_t op, dst, src; int16_t off; int32_t imm; };

static void decode(const uint8_t *c, struct inst *i)
{
	i->op = c[0];
	i->dst = c[1] & 15;
	i->src = c[1] >> 4;
	i->off = (int16_t)(uint16_t)(c[2] | c[3] << 8);
	i->imm = (int32_t)rd32(c + 4);
}

static int size_of(uint8_t op) { switch (op & 0x18) { case 0x00: return 4; case 0x08: return 2; case 0x10: return 1; default: return 8; } }

uint32_t ebpf_check(const uint8_t *code, uint32_t n, uint32_t nhelpers)
{
	for (uint32_t pc = 0; pc < n; pc++) {
		struct inst i;
		decode(code + pc * 8, &i);
		uint8_t op = i.op, cls = CLS(op), code4 = op >> 4;
		int ok = i.dst <= 10 && i.src <= 10;
		switch (cls) {
		case C_LD:
			ok = ok && op == 0x18 && i.src == 0 && pc + 1 < n && i.dst != 10;
			if (ok) {
				struct inst j;
				decode(code + (pc + 1) * 8, &j);
				ok = j.op == 0 && j.dst == 0 && j.src == 0 && j.off == 0;
				pc++;
			}
			break;
		case C_LDX:
			ok = ok && ((op & 0xe0) == 0x60 || ((op & 0xe0) == 0x80 && (op & 0x18) != 0x18)) && i.dst != 10;
			break;
		case C_ST:
			ok = ok && (op & 0xe0) == 0x60 && i.src == 0;
			break;
		case C_STX:
			if ((op & 0xe0) == 0x60)
				break;
			ok = ok && (op & 0xe0) == 0xc0 && ((op & 0x18) == 0x00 || (op & 0x18) == 0x18) &&
			     (i.imm == 0x00 || i.imm == 0x01 || i.imm == 0x40 || i.imm == 0x41 || i.imm == 0x50 || i.imm == 0x51 ||
			      i.imm == 0xa0 || i.imm == 0xa1 || i.imm == 0xe1 || i.imm == 0xf1);
			break;
		case C_ALU:
		case C_ALU64:
			ok = ok && i.dst != 10 && code4 <= 13;
			if (code4 == 8)                                    /* neg */
				ok = ok && !(op & 8);
			if (code4 == 13)                                   /* le/be (ALU), bswap (ALU64) */
				ok = ok && (i.imm == 16 || i.imm == 32 || i.imm == 64) && (cls == C_ALU || !(op & 8));
			if (code4 == 11 && i.off)                          /* movsx */
				ok = ok && (op & 8) && (i.off == 8 || i.off == 16 || (i.off == 32 && cls == C_ALU64));
			if ((code4 == 3 || code4 == 9) && i.off)           /* sdiv/smod */
				ok = ok && i.off == 1;
			break;
		default: {                                             /* JMP, JMP32 */
			int64_t t;
			if (code4 == 0) {                                  /* ja, ja32 */
				t = (int64_t)pc + 1 + (cls == C_JMP ? i.off : i.imm);
				ok = ok && t >= 0 && t < n && !(op & 8);
			} else if (code4 == 8) {                           /* call */
				ok = ok && cls == C_JMP;
				if (op & 8)
					ok = ok && i.dst <= 9;                     /* callx: the helper number in dst */
				else if (i.src == 1) {
					t = (int64_t)pc + 1 + i.imm;
					ok = ok && t >= 0 && t < n;
				} else
					ok = ok && i.src == 0 && (uint32_t)i.imm < nhelpers;
			} else if (code4 == 9) {
				ok = ok && cls == C_JMP;                       /* exit */
			} else {
				t = (int64_t)pc + 1 + i.off;
				ok = ok && code4 <= 13 && t >= 0 && t < n;
			}
		}
		}
		if (!ok)
			return pc + 1;
	}
	return 0;
}

int ebpf_region(struct ebpf_vm *vm, uint32_t va, uint32_t len, void *p, int writable)
{
	if (vm->nregions >= EBPF_MAX_REGIONS)
		return -1;
	struct ebpf_region *r = &vm->region[vm->nregions];
	r->va = va, r->len = len, r->p = (uint8_t *)p, r->writable = writable ? 1u : 0u;
	return (int)vm->nregions++;
}

uint8_t *ebpf_mem(struct ebpf_vm *vm, uint64_t va, uint32_t len, int write)
{
	for (uint32_t k = 0; k < vm->nregions; k++) {
		struct ebpf_region *r = &vm->region[k];
		if (va >= r->va && va - r->va <= r->len && len <= r->len - (uint32_t)(va - r->va) && (!write || r->writable))
			return r->p + (uint32_t)(va - r->va);
	}
	return 0;
}

void ebpf_stop(struct ebpf_vm *vm, uint32_t error)
{
	if (!vm->error)
		vm->error = error ? error : EBPF_E_STOPPED;
}

static uint64_t load(const uint8_t *p, int n)
{
	uint64_t v = 0;
	for (int k = n - 1; k >= 0; k--)
		v = v << 8 | p[k];
	return v;
}

static void store(uint8_t *p, int n, uint64_t v)
{
	for (int k = 0; k < n; k++, v >>= 8)
		p[k] = (uint8_t)v;
}

static uint64_t bswap(uint64_t v, int bits)
{
	uint64_t r = 0;
	for (int k = 0; k < bits / 8; k++, v >>= 8)
		r = r << 8 | (v & 0xff);
	return r;
}

static uint64_t sext(uint64_t v, int bits) { return bits == 64 ? v : (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits)); }

static uint64_t alu(uint32_t code4, int is64, uint64_t a, uint64_t b, int16_t off, uint32_t imm, int *bad)
{
	if (!is64) {
		a &= 0xffffffffu;
		b &= 0xffffffffu;
	}
	uint64_t r;
	switch (code4) {
	case 0: r = a + b; break;
	case 1: r = a - b; break;
	case 2: r = a * b; break;
	case 3:
		if (off) {                                             /* sdiv: by zero 0; INT_MIN / -1 overflows to INT_MIN */
			int64_t x = (int64_t)sext(a, is64 ? 64 : 32), y = (int64_t)sext(b, is64 ? 64 : 32);
			r = y == 0 ? 0 : (y == -1 ? (uint64_t)(0 - (uint64_t)x) : (uint64_t)(x / y));
		} else
			r = b ? a / b : 0;
		break;
	case 4: r = a | b; break;
	case 5: r = a & b; break;
	case 6: r = a << (b & (is64 ? 63 : 31)); break;
	case 7: r = a >> (b & (is64 ? 63 : 31)); break;
	case 8: r = 0 - a; break;
	case 9:
		if (off) {                                             /* smod: by zero the dividend; INT_MIN % -1 is 0 */
			int64_t x = (int64_t)sext(a, is64 ? 64 : 32), y = (int64_t)sext(b, is64 ? 64 : 32);
			r = y == 0 ? (uint64_t)x : (y == -1 ? 0 : (uint64_t)(x % y));
		} else
			r = b ? a % b : a;
		break;
	case 10: r = a ^ b; break;
	case 11: r = off ? sext(b, off) : b; break;              /* mov, movsx */
	case 12:
		r = is64 ? (uint64_t)((int64_t)a >> (b & 63)) : (uint64_t)(uint32_t)((int32_t)(uint32_t)a >> (b & 31));
		break;
	case 13: r = 0; (void)imm; break;                        /* byte swaps: handled by the caller */
	default: *bad = 1; r = 0;
	}
	return is64 ? r : (r & 0xffffffffu);
}

static int cond(uint32_t code4, int is64, uint64_t a, uint64_t b)
{
	if (!is64) {
		a &= 0xffffffffu;
		b &= 0xffffffffu;
	}
	int64_t sa = is64 ? (int64_t)a : (int64_t)(int32_t)(uint32_t)a, sb = is64 ? (int64_t)b : (int64_t)(int32_t)(uint32_t)b;
	switch (code4) {
	case 1: return a == b;
	case 2: return a > b;
	case 3: return a >= b;
	case 4: return (a & b) != 0;
	case 5: return a != b;
	case 6: return sa > sb;
	case 7: return sa >= sb;
	case 10: return a < b;
	case 11: return a <= b;
	case 12: return sa < sb;
	case 13: return sa <= sb;
	}
	return 0;
}

uint64_t ebpf_run(struct ebpf_vm *vm, uint32_t pc, const uint64_t args[5])
{
	uint64_t *reg = vm->reg;
	struct { uint32_t ret; uint64_t r6, r7, r8, r9, r10; } frames[EBPF_MAX_DEPTH];
	uint32_t depth = 0;

	vm->error = 0, vm->error_pc = 0, vm->executed = 0;
	if (vm->nregions == 0)
		ebpf_region(vm, EBPF_STACK_VA, EBPF_STACK, vm->stack, 1);
	for (int k = 0; k < 11; k++)
		reg[k] = 0;
	for (int k = 0; k < 5; k++)
		reg[k + 1] = args ? args[k] : 0;
	reg[10] = EBPF_STACK_VA + EBPF_STACK;                     /* the first frame is the top of the stack */

	for (;;) {
		if (pc >= vm->ninsts) {
			vm->error = EBPF_E_JUMP;
			break;
		}
		if (vm->executed >= vm->budget) {
			vm->error = EBPF_E_BUDGET;
			break;
		}
		vm->executed++;
		if (vm->trace)
			vm->trace(vm, pc, reg);
		struct inst i;
		decode(vm->code + pc * 8, &i);
		uint8_t op = i.op, cls = CLS(op);
		uint32_t code4 = op >> 4;
		uint32_t here = pc++;
		vm->error_pc = here;
		int bad = 0;
		switch (cls) {
		case C_ALU:
		case C_ALU64: {
			int is64 = cls == C_ALU64;
			if (code4 == 13) {                                 /* le / be (ALU), bswap (ALU64) */
				uint64_t v = reg[i.dst];
				int bits = i.imm;
				if (is64 || (op & 8))
					v = bswap(v, bits);
				else
					v = bits == 64 ? v : v & (((uint64_t)1 << bits) - 1);
				reg[i.dst] = v;
				break;
			}
			uint64_t b = (op & 8) ? reg[i.src] : (uint64_t)(int64_t)i.imm;
			if (code4 == 11 && i.off && !is64)                 /* movsx into 32 bits */
				reg[i.dst] = sext(b, i.off) & 0xffffffffu;
			else
				reg[i.dst] = alu(code4, is64, reg[i.dst], b, i.off, (uint32_t)i.imm, &bad);
			break;
		}
		case C_LD: {                                           /* lddw */
			uint64_t hi = rd32(vm->code + pc * 8 + 4);
			reg[i.dst] = (uint64_t)(uint32_t)i.imm | hi << 32;
			pc++;
			break;
		}
		case C_LDX: {
			int n = size_of(op);
			uint8_t *p = ebpf_mem(vm, reg[i.src] + (uint64_t)(int64_t)i.off, (uint32_t)n, 0);
			if (!p) {
				vm->error = EBPF_E_MEMORY;
				goto out;
			}
			uint64_t v = load(p, n);
			reg[i.dst] = (op & 0xe0) == 0x80 ? sext(v, n * 8) : v;
			break;
		}
		case C_ST:
		case C_STX: {
			int n = size_of(op);
			uint8_t *p = ebpf_mem(vm, reg[i.dst] + (uint64_t)(int64_t)i.off, (uint32_t)n, 1);
			if (!p) {
				vm->error = EBPF_E_MEMORY;
				goto out;
			}
			if ((op & 0xe0) == 0x60) {
				store(p, n, cls == C_ST ? (uint64_t)(int64_t)i.imm : reg[i.src]);
				break;
			}
			/* atomics (one thread: plain read-modify-write) */
			uint64_t old = load(p, n), s = reg[i.src], v;
			uint32_t a = (uint32_t)i.imm;
			if (n == 4)
				s &= 0xffffffffu;
			if (a == 0xe1) {                                   /* xchg */
				store(p, n, s);
				reg[i.src] = old;
				break;
			}
			if (a == 0xf1) {                                   /* cmpxchg: compare with r0 */
				uint64_t r0 = n == 4 ? reg[0] & 0xffffffffu : reg[0];
				if (r0 == old)
					store(p, n, s);
				reg[0] = old;
				break;
			}
			switch (a & 0xf0) {
			case 0x00: v = old + s; break;
			case 0x40: v = old | s; break;
			case 0x50: v = old & s; break;
			default: v = old ^ s; break;
			}
			store(p, n, v);
			if (a & 1)
				reg[i.src] = old;
			break;
		}
		default: {                                             /* JMP, JMP32 */
			int is64 = cls == C_JMP;
			if (code4 == 0) {
				pc = pc + (uint32_t)(is64 ? (int32_t)i.off : i.imm);
				break;
			}
			if (code4 == 9) {                                  /* exit */
				if (depth == 0)
					goto out;
				depth--;
				pc = frames[depth].ret;
				reg[6] = frames[depth].r6, reg[7] = frames[depth].r7, reg[8] = frames[depth].r8;
				reg[9] = frames[depth].r9, reg[10] = frames[depth].r10;
				break;
			}
			if (code4 == 8) {                                  /* call */
				if (!(op & 8) && i.src == 1) {                 /* a local call: a new stack frame */
					if (depth >= EBPF_MAX_DEPTH - 1) {
						vm->error = EBPF_E_DEPTH;
						goto out;
					}
					frames[depth].ret = pc;
					frames[depth].r6 = reg[6], frames[depth].r7 = reg[7], frames[depth].r8 = reg[8];
					frames[depth].r9 = reg[9], frames[depth].r10 = reg[10];
					depth++;
					reg[10] -= EBPF_FRAME;
					pc = pc + (uint32_t)i.imm;
					break;
				}
				uint64_t n = (op & 8) ? reg[i.dst] : (uint64_t)(uint32_t)i.imm;
				if (n >= vm->nhelpers || !vm->helpers[n]) {
					vm->error = EBPF_E_HELPER;
					goto out;
				}
				reg[0] = vm->helpers[n](vm, reg[1], reg[2], reg[3], reg[4], reg[5]);
				if (vm->error)
					goto out;
				if (vm->unwind_helper && n == vm->unwind_helper && reg[0] == 0)
					goto out;
				break;
			}
			uint64_t b = (op & 8) ? reg[i.src] : (uint64_t)(int64_t)i.imm;
			if (cond(code4, is64, reg[i.dst], b))
				pc = pc + (uint32_t)(int32_t)i.off;
		}
		}
		if (bad) {
			vm->error = EBPF_E_OPCODE;
			break;
		}
	}
out:
	return vm->error ? 0 : reg[0];
}
