// The attract-line composition (effects, FLC effect slots, outlines, the 8 frame buffers) against the Python sign
// emulation (test/make_effect_fixtures.py): every frame of every case identical.
import { test } from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"
import { composeAttract, Effects, img, TextLine } from "../src/index.ts"
import { fixtures, loadFonts, ROOT, sha } from "./load.ts"

const fx0 = fixtures("attract.fixtures.json")
const fonts = await loadFonts(fx0.fonts)
const slots = new Map<number, Uint8Array>()
for (const n of fx0.slots)
  slots.set(n, new Uint8Array(readFileSync(join(ROOT, "studio", "content", "animations", `effect${String(n).padStart(2, "0")}.flc`))))

for (const c of fx0.cases) {
  test(c.name, () => {
    const fx = new Effects(slots, 0)
    const data = Buffer.from(c.data, "hex")
    const table = new Uint8Array(256)
    table.set(data)
    const tl = new TextLine(table, fonts, c.W, c.H, 0)
    const ring = Array.from({ length: 8 }, () => img(c.W, c.H))
    const states: number[] = [], shas: string[] = []
    for (let k = 0; k < 400; k++) {
      const st = tl.step()
      const frame = ring[k % 8]
      composeAttract(fx, frame, tl.img, tl.f, { data, effect: c.effect, outlineStyle: c.style, outlineColour: c.colour })
      fx.frameCount++
      shas.push(sha(frame))
      states.push(st)
      if (st === 4) break
    }
    assert.deepEqual(states, c.states, "states")
    const first = shas.findIndex((h, i) => h !== c.sha256[i])
    assert.equal(first, -1, `frame ${first} differs`)
  })
}
