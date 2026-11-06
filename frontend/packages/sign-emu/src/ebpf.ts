// The eBPF interpreter of programmable apps (docs/APPS.md): a line-by-line port of firmware/ext/ebpf.c, so the
// Studio runs an app's bytecode exactly as the sign does. RFC 9669, little-endian memory, interpreted. Both pass every
// bpf-conformance test program (test/ebpf_conformance.tsv) and must give identical instruction traces
// (test/ebpf.test.ts). Registers are 64-bit BigInts, masked after every operation.

export const EBPF_FRAME = 512
export const EBPF_MAX_DEPTH = 8
export const EBPF_STACK = EBPF_FRAME * EBPF_MAX_DEPTH
export const EBPF_STACK_VA = 0x10000000

export const EbpfError = { OK: 0, OPCODE: 1, MEMORY: 2, BUDGET: 3, HELPER: 4, DEPTH: 5, JUMP: 6, STOPPED: 7 } as const

const M64 = (1n << 64n) - 1n
const M32 = 0xffffffffn
const u64 = (v: bigint) => v & M64
const s64 = (v: bigint) => BigInt.asIntN(64, v)
const sext = (v: bigint, bits: number) => (bits === 64 ? u64(v) : u64(BigInt.asIntN(bits, v)))

export type EbpfHelper = (vm: EbpfVm, a: bigint, b: bigint, c: bigint, d: bigint, e: bigint) => bigint

export interface EbpfRegion { va: number; len: number; p: Uint8Array; writable: boolean }

interface Inst { op: number; dst: number; src: number; off: number; imm: number }

function decode(code: Uint8Array, pc: number): Inst {
  const o = pc * 8
  const off = code[o + 2] | (code[o + 3] << 8)
  const imm = code[o + 4] | (code[o + 5] << 8) | (code[o + 6] << 16) | (code[o + 7] << 24)
  return { op: code[o], dst: code[o + 1] & 15, src: code[o + 1] >> 4, off: off >= 0x8000 ? off - 0x10000 : off, imm: imm | 0 }
}

const sizeOf = (op: number) => [4, 2, 1, 8][(op & 0x18) >> 3]

/** ebpf_check: 0 if the program is valid, else the index of the first bad instruction + 1. */
export function ebpfCheck(code: Uint8Array, n: number, nhelpers: number): number {
  for (let pc = 0; pc < n; pc++) {
    const i = decode(code, pc), op = i.op, cls = op & 7, code4 = op >> 4
    let ok = i.dst <= 10 && i.src <= 10
    switch (cls) {
      case 0:
        ok = ok && op === 0x18 && i.src === 0 && pc + 1 < n && i.dst !== 10
        if (ok) {
          const j = decode(code, pc + 1)
          ok = j.op === 0 && j.dst === 0 && j.src === 0 && j.off === 0
          pc++
        }
        break
      case 1:
        ok = ok && ((op & 0xe0) === 0x60 || ((op & 0xe0) === 0x80 && (op & 0x18) !== 0x18)) && i.dst !== 10
        break
      case 2:
        ok = ok && (op & 0xe0) === 0x60 && i.src === 0
        break
      case 3:
        if ((op & 0xe0) === 0x60) break
        ok = ok && (op & 0xe0) === 0xc0 && ((op & 0x18) === 0x00 || (op & 0x18) === 0x18) &&
          [0x00, 0x01, 0x40, 0x41, 0x50, 0x51, 0xa0, 0xa1, 0xe1, 0xf1].includes(i.imm)
        break
      case 4:
      case 7:
        ok = ok && i.dst !== 10 && code4 <= 13
        if (code4 === 8) ok = ok && !(op & 8)
        if (code4 === 13) ok = ok && (i.imm === 16 || i.imm === 32 || i.imm === 64) && (cls === 4 || !(op & 8))
        if (code4 === 11 && i.off) ok = ok && !!(op & 8) && (i.off === 8 || i.off === 16 || (i.off === 32 && cls === 7))
        if ((code4 === 3 || code4 === 9) && i.off) ok = ok && i.off === 1
        break
      default: {
        let t: number
        if (code4 === 0) {
          t = pc + 1 + (cls === 5 ? i.off : i.imm)
          ok = ok && t >= 0 && t < n && !(op & 8)
        } else if (code4 === 8) {
          ok = ok && cls === 5
          if (op & 8) ok = ok && i.dst <= 9
          else if (i.src === 1) {
            t = pc + 1 + i.imm
            ok = ok && t >= 0 && t < n
          } else ok = ok && i.src === 0 && i.imm >>> 0 < nhelpers
        } else if (code4 === 9) {
          ok = ok && cls === 5
        } else {
          t = pc + 1 + i.off
          ok = ok && code4 <= 13 && t >= 0 && t < n
        }
      }
    }
    if (!ok) return pc + 1
  }
  return 0
}

function load(p: Uint8Array, at: number, n: number): bigint {
  let v = 0n
  for (let k = n - 1; k >= 0; k--) v = (v << 8n) | BigInt(p[at + k])
  return v
}

function store(p: Uint8Array, at: number, n: number, v: bigint) {
  for (let k = 0; k < n; k++, v >>= 8n) p[at + k] = Number(v & 0xffn)
}

function bswap(v: bigint, bits: number): bigint {
  let r = 0n
  for (let k = 0; k < bits / 8; k++, v >>= 8n) r = (r << 8n) | (v & 0xffn)
  return r
}

function alu(code4: number, is64: boolean, a: bigint, b: bigint, off: number): bigint | null {
  if (!is64) {
    a &= M32
    b &= M32
  }
  const bits = is64 ? 64 : 32
  let r: bigint
  switch (code4) {
    case 0: r = a + b; break
    case 1: r = a - b; break
    case 2: r = a * b; break
    case 3:
      if (off) {
        const x = BigInt.asIntN(64, sext(a, bits)), y = BigInt.asIntN(64, sext(b, bits))
        r = y === 0n ? 0n : y === -1n ? -x : x / y
      } else r = b ? a / b : 0n
      break
    case 4: r = a | b; break
    case 5: r = a & b; break
    case 6: r = a << (b & (is64 ? 63n : 31n)); break
    case 7: r = a >> (b & (is64 ? 63n : 31n)); break
    case 8: r = -a; break
    case 9:
      if (off) {
        const x = BigInt.asIntN(64, sext(a, bits)), y = BigInt.asIntN(64, sext(b, bits))
        r = y === 0n ? x : y === -1n ? 0n : x % y
      } else r = b ? a % b : a
      break
    case 10: r = a ^ b; break
    case 11: r = off ? sext(b, off) : b; break
    case 12:
      r = is64 ? s64(a) >> (b & 63n) : BigInt(BigInt.asIntN(32, a) >> (b & 31n)) & M32
      break
    default: return null
  }
  return is64 ? u64(r) : u64(r) & M32
}

function cond(code4: number, is64: boolean, a: bigint, b: bigint): boolean {
  if (!is64) {
    a &= M32
    b &= M32
  }
  const sa = is64 ? s64(a) : BigInt.asIntN(32, a), sb = is64 ? s64(b) : BigInt.asIntN(32, b)
  switch (code4) {
    case 1: return a === b
    case 2: return a > b
    case 3: return a >= b
    case 4: return (a & b) !== 0n
    case 5: return a !== b
    case 6: return sa > sb
    case 7: return sa >= sb
    case 10: return a < b
    case 11: return a <= b
    case 12: return sa < sb
    case 13: return sa <= sb
  }
  return false
}

export class EbpfVm {
  code: Uint8Array
  ninsts: number
  helpers: (EbpfHelper | null)[]
  regions: EbpfRegion[] = []
  budget = 1_000_000
  executed = 0
  error = 0
  errorPc = 0
  unwindHelper = 0
  trace: ((pc: number, reg: bigint[]) => void) | null = null
  reg: bigint[] = Array(11).fill(0n)
  stack = new Uint8Array(EBPF_STACK)

  constructor(code: Uint8Array, helpers: (EbpfHelper | null)[] = []) {
    this.code = code
    this.ninsts = code.length >> 3
    this.helpers = helpers
  }

  /** ebpf_region */
  region(va: number, len: number, p: Uint8Array, writable: boolean) {
    this.regions.push({ va, len, p, writable })
  }

  /** ebpf_mem: [region bytes, offset] for [va, va + len), or null */
  mem(va: bigint, len: number, write: boolean): [Uint8Array, number] | null {
    for (const r of this.regions) {
      const rva = BigInt(r.va)
      if (va >= rva && va - rva <= BigInt(r.len) && BigInt(len) <= BigInt(r.len) - (va - rva) && (!write || r.writable))
        return [r.p, Number(va - rva)]
    }
    return null
  }

  stop(error: number = EbpfError.STOPPED) {
    if (!this.error) this.error = error || EbpfError.STOPPED
  }

  /** ebpf_run: r0, or 0n with this.error set */
  run(pc: number, args: bigint[] = []): bigint {
    const reg = this.reg
    const frames: { ret: number; r6: bigint; r7: bigint; r8: bigint; r9: bigint; r10: bigint }[] = []
    this.error = 0
    this.errorPc = 0
    this.executed = 0
    if (!this.regions.length) this.region(EBPF_STACK_VA, EBPF_STACK, this.stack, true)
    for (let k = 0; k < 11; k++) reg[k] = 0n
    for (let k = 0; k < 5; k++) reg[k + 1] = args[k] ?? 0n
    reg[10] = BigInt(EBPF_STACK_VA + EBPF_STACK)
    out: for (;;) {
      if (pc >= this.ninsts || pc < 0) {
        this.error = EbpfError.JUMP
        break
      }
      if (this.executed >= this.budget) {
        this.error = EbpfError.BUDGET
        break
      }
      this.executed++
      if (this.trace) this.trace(pc, reg)
      const i = decode(this.code, pc), op = i.op, cls = op & 7, code4 = op >> 4
      const here = pc++
      this.errorPc = here
      switch (cls) {
        case 4:
        case 7: {
          const is64 = cls === 7
          if (code4 === 13) {
            let v = reg[i.dst]
            if (is64 || op & 8) v = bswap(v, i.imm)
            else v = i.imm === 64 ? v : v & ((1n << BigInt(i.imm)) - 1n)
            reg[i.dst] = v
            break
          }
          const b = op & 8 ? reg[i.src] : u64(BigInt(i.imm))
          let r: bigint | null
          if (code4 === 11 && i.off && !is64) r = sext(b, i.off) & M32
          else r = alu(code4, is64, reg[i.dst], b, i.off)
          if (r === null) {
            this.error = EbpfError.OPCODE
            break out
          }
          reg[i.dst] = r
          break
        }
        case 0: {
          const o = pc * 8
          const hi = BigInt((this.code[o + 4] | (this.code[o + 5] << 8) | (this.code[o + 6] << 16) | (this.code[o + 7] << 24)) >>> 0)
          reg[i.dst] = BigInt(i.imm >>> 0) | (hi << 32n)
          pc++
          break
        }
        case 1: {
          const n = sizeOf(op)
          const m = this.mem(u64(reg[i.src] + BigInt(i.off)), n, false)
          if (!m) {
            this.error = EbpfError.MEMORY
            break out
          }
          const v = load(m[0], m[1], n)
          reg[i.dst] = (op & 0xe0) === 0x80 ? sext(v, n * 8) : v
          break
        }
        case 2:
        case 3: {
          const n = sizeOf(op)
          const m = this.mem(u64(reg[i.dst] + BigInt(i.off)), n, true)
          if (!m) {
            this.error = EbpfError.MEMORY
            break out
          }
          if ((op & 0xe0) === 0x60) {
            store(m[0], m[1], n, cls === 2 ? u64(BigInt(i.imm)) : reg[i.src])
            break
          }
          const old = load(m[0], m[1], n)
          let s = reg[i.src]
          const a = i.imm >>> 0
          if (n === 4) s &= M32
          if (a === 0xe1) {
            store(m[0], m[1], n, s)
            reg[i.src] = old
            break
          }
          if (a === 0xf1) {
            const r0 = n === 4 ? reg[0] & M32 : reg[0]
            if (r0 === old) store(m[0], m[1], n, s)
            reg[0] = old
            break
          }
          let v: bigint
          switch (a & 0xf0) {
            case 0x00: v = old + s; break
            case 0x40: v = old | s; break
            case 0x50: v = old & s; break
            default: v = old ^ s
          }
          store(m[0], m[1], n, u64(v))
          if (a & 1) reg[i.src] = old
          break
        }
        default: {
          const is64 = cls === 5
          if (code4 === 0) {
            pc = pc + (is64 ? i.off : i.imm)
            break
          }
          if (code4 === 9) {
            if (!frames.length) break out
            const f = frames.pop()!
            pc = f.ret
            reg[6] = f.r6; reg[7] = f.r7; reg[8] = f.r8; reg[9] = f.r9; reg[10] = f.r10
            break
          }
          if (code4 === 8) {
            if (!(op & 8) && i.src === 1) {
              if (frames.length >= EBPF_MAX_DEPTH - 1) {
                this.error = EbpfError.DEPTH
                break out
              }
              frames.push({ ret: pc, r6: reg[6], r7: reg[7], r8: reg[8], r9: reg[9], r10: reg[10] })
              reg[10] -= BigInt(EBPF_FRAME)
              pc = pc + i.imm
              break
            }
            const n = op & 8 ? reg[i.dst] : BigInt(i.imm >>> 0)
            const h = n < BigInt(this.helpers.length) ? this.helpers[Number(n)] : null
            if (!h) {
              this.error = EbpfError.HELPER
              break out
            }
            reg[0] = u64(h(this, reg[1], reg[2], reg[3], reg[4], reg[5]))
            if (this.error) break out
            if (this.unwindHelper && n === BigInt(this.unwindHelper) && reg[0] === 0n) break out
            break
          }
          const b = op & 8 ? reg[i.src] : u64(BigInt(i.imm))
          if (cond(code4, is64, reg[i.dst], b)) pc = pc + i.off
        }
      }
    }
    return this.error ? 0n : reg[0]
  }
}
