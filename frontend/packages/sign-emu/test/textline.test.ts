// The text line against the Python sign emulation (test/make_fixtures.py): every frame of every case identical.
import { test } from "node:test"
import assert from "node:assert/strict"
import { textlineFrames } from "../src/index.ts"
import { fixtures, loadFonts, sha } from "./load.ts"

const fx = fixtures("textline.fixtures.json")
const fonts = await loadFonts(fx.fonts)

for (const c of fx.cases) {
  test(c.name, () => {
    const data = new Uint8Array([...Buffer.from(c.data, "hex"), 0])
    const { frames, states } = textlineFrames(data, fonts, c.W, c.H, 0, 800)
    assert.deepEqual(states, c.states, "states")
    const got = frames.map(sha)
    const first = got.findIndex((h, i) => h !== c.sha256[i])
    assert.equal(first, -1, `frame ${first} differs`)
  })
}
