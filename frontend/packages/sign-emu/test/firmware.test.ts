// The attract loop against the REAL firmware: umami-marquee-sim-test attractdump runs the firmware in the C++ simulator
// with four attract lines (a plain scroll, a centred held line, a line in diagonal stripes, a line with a word in each
// per-character motion: wave, bounce, shake, blink) on this sign's 200 x 16
// and dumps every frame the display loop draws with the firmware's frame counter at that moment. The player must
// reproduce them, frame for frame, across the line changes and round to the first line again. Make the dump first:
//     build/simulator/umami-marquee-sim-test attractdump build/sim-test/attractdump
import { test } from "node:test"
import assert from "node:assert/strict"
import { existsSync, readdirSync, readFileSync } from "node:fs"
import { join } from "node:path"
import { AttractPlayer, Effects } from "../src/index.ts"
import { loadFonts, ROOT } from "./load.ts"

const DIR = join(ROOT, "build", "sim-test", "attractdump")
const have = existsSync(join(DIR, "drawn.bin"))

test("the attract loop matches the firmware frame for frame", { skip: !have && "no dump: run umami-marquee-sim-test attractdump" }, async () => {
  const meta = JSON.parse(readFileSync(join(DIR, "meta.json"), "utf8"))
  // our firmware's fonts, in its order (the stock ones it keeps, then the MQF fonts by name), and the ones its
  // automatic picks may choose (firmware/auto_fonts.txt): a line that names no font gets its pick
  const listOf = (f: string) => readFileSync(join(ROOT, "firmware", f), "utf8").split("\n").map((l) => l.replace(/#.*/, "").trim()).filter(Boolean)
  const kept = new Set(listOf("stock_fonts.txt")), auto = new Set(listOf("auto_fonts.txt"))
  const files = readdirSync(join(ROOT, "studio", "content", "fonts"))
  const fonts = await loadFonts([...files.filter((n) => /^fonte\d\d\.png$/.test(n) && kept.has(n)).sort(), ...files.filter((n) => n.endsWith(".mqf")).sort()])
  const lines = Array.from({ length: 16 }, () => null as null | { data: Uint8Array; effect: number })
  for (const l of meta.lines) lines[l.line] = { data: Buffer.from(l.data, "hex"), effect: l.effect >>> 0 }
  // every frame the firmware drew, with its frame counter at that moment (drawn.bin): the effects' phase, exactly
  const bin = readFileSync(join(DIR, "drawn.bin"))
  const fw: { fc: number; d: Uint16Array }[] = []
  for (let o = 0; o + 6404 <= bin.length; o += 6404) {
    const d = new Uint16Array(3200)
    for (let i = 0; i < 3200; i++) d[i] = bin.readUInt16BE(o + 4 + 2 * i) & 0x7fff
    fw.push({ fc: bin.readUInt32LE(o), d })
  }
  const same = (a: Uint16Array, b: Uint16Array) => a.every((v, i) => v === b[i])
  // where the attract loop starts (line 0's first frame), then every drawn frame from there identical
  let best = { start: -1, upto: 0, played: new Set<number>() }
  for (let o = 0; o < 300 && best.upto < 100; o++) {
    const fx = new Effects(null, 0)
    const p = new AttractPlayer(fonts, meta.w, meta.h, lines, fx, undefined, auto)
    const played = new Set<number>()
    let k = o
    for (; k < fw.length; k++) {
      fx.frameCount = fw[k].fc
      const f = p.frame()
      played.add(p.line)
      if (!same(f.d, fw[k].d)) break
    }
    if (k - o > best.upto) best = { start: o, upto: k - o, played }
  }
  const n = fw.length - best.start
  console.log(`attract starts at drawn frame ${best.start}; ${best.upto} of ${n} drawn frames identical; lines played: ${[...best.played].join(", ")}`)
  assert.equal(best.upto, n, `drawn frame ${best.start + best.upto} differs`)
  assert.ok(best.played.size === 4, "all four lines were played")
})
