# bpf-conformance test programs

`tests/` is the test suite of [microsoft/bpf-conformance](https://github.com/microsoft/bpf-conformance) at commit
698e379 (MIT licence, `LICENSE`): eBPF programs in assembly, each with its input memory and expected result.

Both of our eBPF interpreters (the sign's, `firmware/ext/ebpf.c`, and the web Studio's,
`frontend/packages/sign-emu/src/ebpf.ts`) must pass every one: `tools/ebpf_asm.py` assembles them into
`frontend/packages/sign-emu/test/ebpf_conformance.tsv`, which both test harnesses run.
