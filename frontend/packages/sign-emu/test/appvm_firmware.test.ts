// Programmable apps against the REAL firmware: make_appvm_dump.sh builds the apps of appvm_scenario.json (clock in a
// font of its own with the wipe and an outline, scrolling text with emoji (one an imported, animated GIF) and a drop shadow, weather from pushed data,
// countdown, counter) as the Studio does, uploads them through our firmware's console in the C++ simulator into the
// content store, and records every frame the display loop draws. The Studio's emulation (appvm.ts + attract.ts) runs
// the same bundles and must draw the same frames.
//     frontend/packages/sign-emu/test/make_appvm_dump.sh
import { test } from "node:test"
import assert from "node:assert/strict"
import { existsSync, readdirSync, readFileSync } from "node:fs"
import { join } from "node:path"
import { AppInstance, AttractPlayer, Effects, parseBundle, unpackEmojiSheet, type AttractLine } from "../src/index.ts"
import { fixtures, loadFonts, ROOT } from "./load.ts"

const DIR = join(ROOT, "build", "sim-test", "appvmdump")
const have = existsSync(join(DIR, "drawn.bin"))

test("apps: the emulation draws what the firmware draws", { skip: !have && "no dump: run test/make_appvm_dump.sh" }, async () => {
  const spec = fixtures("appvm_scenario.json")
  const T = +readFileSync(join(DIR, "time"), "utf8")
  const fontFiles = readdirSync(join(ROOT, "studio", "content", "fonts"))
  // the firmware's order: its MQF fonts by name (fonts.lst)
  const auto = new Set(readFileSync(join(ROOT, "firmware", "auto_fonts.txt"), "utf8").split("\n").map((l) => l.trim()).filter((l) => l && !l.startsWith("#")))
  const names = fontFiles.filter((n) => n.endsWith(".mqf")).sort()
  const fonts = await loadFonts(names)
  // the sheet the firmware reads: the store's emoji.bin (with the scenario's imported, animated emoji), else the boot flash's
  const own = join(DIR, "emoji.bin")
  const sheet = await unpackEmojiSheet(new Uint8Array(readFileSync(existsSync(own) ? own : join(ROOT, "bootloader", "build", "emoji", "emoji.bin"))))
  const bin = readFileSync(join(DIR, "drawn.bin"))
  const fw: { fc: number; d: Uint16Array }[] = []
  for (let o = 0; o + 6404 <= bin.length; o += 6404) {
    const d = new Uint16Array(3200)
    for (let i = 0; i < 3200; i++) d[i] = bin.readUInt16BE(o + 4 + 2 * i) & 0x7fff
    fw.push({ fc: bin.readUInt32LE(o), d })
  }
  const bundles = new Map<number, Awaited<ReturnType<typeof parseBundle>>>()
  for (const p of spec.placements) bundles.set(p.line, await parseBundle(new Uint8Array(readFileSync(join(DIR, `app${p.line}.mqap`)))))
  // sign time: set just before the capture (the dump's "time" command); whole minutes are all the apps show
  const fx = new Effects(null, 0)
  const make = () => {
    const lines: (AttractLine | null)[] = Array(16).fill(null)
    for (const p of spec.placements) {
      const app = new AppInstance(bundles.get(p.line)!,
        { fonts, fontNames: names, sheet, fx, now: () => T, auto }, 200, 16)
      for (const [k, v] of Object.entries((spec.data?.[String(p.line)] ?? {}) as Record<string, string>))
        app.data.set(k, { value: new TextEncoder().encode(v), at: T })
      assert.ok(app.init(), `app on line ${p.line}: ${app.error}`)
      lines[p.line] = { data: new Uint8Array([0x20]), effect: 0x80008000, program: app }
    }
    return new AttractPlayer(fonts, 200, 16, lines, fx, sheet)
  }
  const same = (a: Uint16Array, b: Uint16Array) => a.every((v, i) => v === b[i])
  let best = { upto: -1, start: 0, first: 0, played: new Set<number>(), diff: "" }
  for (let o = 0; o < 300 && best.upto < 100; o++)
    for (const first of spec.placements.map((p: { line: number }) => p.line)) {
      const p = make()
      p.cur = first - 1
      const played = new Set<number>()
      let k = o, diff = ""
      for (; k < fw.length; k++) {
        fx.frameCount = fw[k].fc
        const f = p.frame()
        played.add(p.line)
        if (!same(f.d, fw[k].d)) {
          const i = f.d.findIndex((v, j) => v !== fw[k].d[j])
          diff = `line ${p.line}, pixel ${i % 200},${Math.floor(i / 200)}: emulation ${f.d[i].toString(16)} firmware ${fw[k].d[i].toString(16)}`
          break
        }
      }
      if (k - o > best.upto) best = { upto: k - o, start: o, first, played, diff }
    }
  if (process.env.APPVM_DEBUG) {
    const p = make()
    p.cur = best.first - 1
    const span = (d: Uint16Array) => { let lo = 999, hi = -1; for (let i = 0; i < d.length; i++) if (d[i]) { lo = Math.min(lo, i % 200); hi = Math.max(hi, i % 200) } return hi < 0 ? "black" : `${lo}..${hi}` }
    for (let k = best.start; k < best.start + best.upto + 6; k++) {
      fx.frameCount = fw[k].fc
      const f = p.frame()
      if (k >= best.start + best.upto - 4) console.log(`drawn ${k}: emu line ${p.line} ${span(f.d)}   fw ${span(fw[k].d)}`)
      if (k === best.start + best.upto)
        for (let y = 0; y < 16; y++) {
          const row = (d: Uint16Array) => Array.from({ length: 12 }, (_, i) => (d[y * 200 + 188 + i] ? (d[y * 200 + 188 + i] === 0x7e80 ? "#" : "o") : ".")).join("")
          console.log(`   row ${String(y).padStart(2)}  emu ${row(f.d)}   fw ${row(fw[k].d)}`)
        }
    }
  }
  const n = fw.length - best.start
  console.log(`line ${best.first} starts at drawn frame ${best.start}; ${best.upto} of ${n} drawn frames identical; lines played: ${[...best.played].join(", ")}${best.diff ? "; first difference: " + best.diff : ""}`)
  assert.equal(best.upto, n, `drawn frame ${best.start + best.upto} differs`)
  assert.equal(best.played.size, spec.placements.length, "every app was shown")
})
