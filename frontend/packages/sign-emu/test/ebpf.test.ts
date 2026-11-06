// The Studio's eBPF interpreter (src/ebpf.ts) on every bpf-conformance test program (test/ebpf_conformance.tsv, made
// by tools/ebpf_asm.py from third_party/bpf-conformance): the same runs as firmware/ext/test/ebpf_conformance.c.
import { test } from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { EbpfVm, ebpfCheck, EBPF_STACK_VA } from "../src/index.ts"

const rows = readFileSync(new URL("./ebpf_conformance.tsv", import.meta.url), "utf8").split("\n").filter((l) => l && !l.startsWith("#"))

test(`ebpf.ts passes all ${rows.length} bpf-conformance tests`, () => {
  const failed: string[] = []
  for (const row of rows) {
    const [name, c, m, r] = row.split("\t")
    const code = Uint8Array.from(Buffer.from(c, "hex")), mem = m === "-" ? new Uint8Array(0) : Uint8Array.from(Buffer.from(m, "hex"))
    const want = BigInt("0x" + r)
    const unwind = (_vm: EbpfVm, a: bigint) => a
    const helpers = [null, null, null, null, null, unwind]
    const bad = ebpfCheck(code, code.length >> 3, helpers.length)
    const vm = new EbpfVm(code, helpers)
    vm.unwindHelper = 5
    vm.region(EBPF_STACK_VA, vm.stack.length, vm.stack, true)
    vm.region(0x20000000, mem.length, mem, true)
    const got = bad ? 0n : vm.run(0, [mem.length ? 0x20000000n : 0n, BigInt(mem.length)])
    if (bad || vm.error || got !== want) failed.push(`${name}: want ${want.toString(16)} got ${got.toString(16)}${bad ? " (rejected)" : ""} error ${vm.error} at ${vm.errorPc}`)
  }
  assert.deepEqual(failed, [])
})
