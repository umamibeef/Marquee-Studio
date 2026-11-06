/* Runs firmware/ext/ebpf.c, natively, over every bpf-conformance test program
 * (frontend/packages/sign-emu/test/ebpf_conformance.tsv, made by tools/ebpf_asm.py): r1 points at the test's memory,
 * r2 is its length, helper 5 is the conformance suite's "unwind" (returns r1; 0 ends the program).
 *     cc -O2 -I firmware/ext firmware/ext/ebpf.c firmware/ext/test/ebpf_conformance.c -o ebpf_conformance
 *     ./ebpf_conformance frontend/packages/sign-emu/test/ebpf_conformance.tsv */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ebpf.h"

static uint64_t unwind(struct ebpf_vm *vm, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	(void)vm, (void)b, (void)c, (void)d, (void)e;
	return a;
}

static int hex(const char *s, uint8_t *out, int max)
{
	int n = 0;
	if (s[0] == '-')
		return 0;
	while (s[0] && s[1] && n < max) {
		unsigned v;
		sscanf(s, "%2x", &v);
		out[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

int main(int argc, char **argv)
{
	FILE *f = fopen(argc > 1 ? argv[1] : "frontend/packages/sign-emu/test/ebpf_conformance.tsv", "r");
	if (!f) {
		perror("tsv");
		return 2;
	}
	static char line[1 << 16];
	static uint8_t code[1 << 14], mem[4096];
	static const ebpf_helper helpers[6] = {0, 0, 0, 0, 0, unwind};
	int pass = 0, fail = 0;
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#')
			continue;
		char *name = strtok(line, "\t"), *c = strtok(0, "\t"), *m = strtok(0, "\t"), *r = strtok(0, "\t\n");
		int nc = hex(c, code, sizeof code), nm = hex(m, mem, sizeof mem);
		unsigned long long want = strtoull(r, 0, 16);
		uint32_t bad = ebpf_check(code, (uint32_t)(nc / 8), 6);
		static struct ebpf_vm vm;
		memset(&vm, 0, sizeof vm);
		vm.code = code, vm.ninsts = (uint32_t)(nc / 8), vm.helpers = helpers, vm.nhelpers = 6, vm.budget = 1000000;
		vm.unwind_helper = 5;
		ebpf_region(&vm, EBPF_STACK_VA, EBPF_STACK, vm.stack, 1);
		ebpf_region(&vm, 0x20000000u, (uint32_t)nm, mem, 1);
		uint64_t args[5] = {nm ? 0x20000000u : 0, (uint64_t)nm, 0, 0, 0};
		uint64_t got = bad ? 0 : ebpf_run(&vm, 0, args);
		if (!bad && !vm.error && got == want)
			pass++;
		else {
			fail++;
			printf("FAIL %-40s want %016llx got %016llx%s error %u at pc %u\n", name, want, (unsigned long long)got,
			       bad ? " (rejected by the check)" : "", vm.error, vm.error_pc);
		}
	}
	printf("ebpf.c: %d of %d conformance tests pass\n", pass, pass + fail);
	return fail != 0;
}
