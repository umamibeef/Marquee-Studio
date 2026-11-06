/* The eBPF interpreter of programmable apps (docs/APPS.md): the instruction set of RFC 9669, little-endian memory,
 * interpreted only. Freestanding C: the same file runs in our firmware (PowerPC) and in the native test harness, and
 * frontend/packages/sign-emu/src/ebpf.ts is its line-by-line port. Both pass every bpf-conformance test program
 * (third_party/bpf-conformance) and must produce identical instruction traces.
 *
 * Memory: programs see a 32-bit virtual address space of regions (the call stack, the app's data, a caller's buffer);
 * every load and store is checked against them. The stack is EBPF_STACK bytes: EBPF_FRAME per call frame, r10 the top
 * of the current frame, at most EBPF_MAX_DEPTH nested local calls. */
#ifndef EBPF_H
#define EBPF_H
#include <stdint.h>

#define EBPF_FRAME      512u
#define EBPF_MAX_DEPTH  8u
#define EBPF_STACK      (EBPF_FRAME * EBPF_MAX_DEPTH)
#define EBPF_STACK_VA   0x10000000u          /* the stack's virtual address */
#define EBPF_MAX_REGIONS 6

enum ebpf_error {
	EBPF_OK = 0,
	EBPF_E_OPCODE,        /* an invalid instruction (the loader rejects these; at run time: never) */
	EBPF_E_MEMORY,        /* a load or store outside every region */
	EBPF_E_BUDGET,        /* the instruction budget ran out */
	EBPF_E_HELPER,        /* a call to a helper that does not exist */
	EBPF_E_DEPTH,         /* local calls nested deeper than EBPF_MAX_DEPTH */
	EBPF_E_JUMP,          /* a jump or call outside the program */
	EBPF_E_STOPPED,       /* a helper stopped the program (its own error) */
};

struct ebpf_vm;
/* A helper: r1..r5 in, r0 out. It may stop the program with ebpf_stop(). */
typedef uint64_t (*ebpf_helper)(struct ebpf_vm *vm, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);

struct ebpf_region { uint32_t va, len; uint8_t *p; uint32_t writable; };

struct ebpf_vm {
	const uint8_t *code;          /* the instructions, 8 bytes each, little-endian */
	uint32_t ninsts;
	const ebpf_helper *helpers;   /* helpers[n] for "call n"; NULL entries do not exist */
	uint32_t nhelpers;
	struct ebpf_region region[EBPF_MAX_REGIONS];
	uint32_t nregions;            /* region[0] is the stack (ebpf_run sets it) */
	uint32_t budget;              /* instructions this run may execute */
	uint32_t executed;            /* instructions it did */
	uint32_t error, error_pc;     /* enum ebpf_error and where */
	uint32_t unwind_helper;       /* a helper whose r0 == 0 ends the program (the conformance tests' helper 5) */
	void (*trace)(struct ebpf_vm *vm, uint32_t pc, const uint64_t *reg);   /* each instruction, before it runs */
	void *user;
	uint64_t reg[11];
	uint8_t stack[EBPF_STACK];
};

/* Check a program before it runs: 0 if it is valid, else the index of the first bad instruction + 1. */
uint32_t ebpf_check(const uint8_t *code, uint32_t ninsts, uint32_t nhelpers);
/* Add a region the program may address (va, len), at host memory p. Returns its index, or -1 when full. */
int ebpf_region(struct ebpf_vm *vm, uint32_t va, uint32_t len, void *p, int writable);
/* Host pointer for [va, va + len) inside one region (writable if asked), or 0. For helpers. */
uint8_t *ebpf_mem(struct ebpf_vm *vm, uint64_t va, uint32_t len, int write);
/* Run from instruction pc with r1..r5 = args; returns r0, or 0 with vm->error set. */
uint64_t ebpf_run(struct ebpf_vm *vm, uint32_t pc, const uint64_t args[5]);
/* From a helper: stop the program with an error (EBPF_E_STOPPED unless another is given). */
void ebpf_stop(struct ebpf_vm *vm, uint32_t error);

#endif
