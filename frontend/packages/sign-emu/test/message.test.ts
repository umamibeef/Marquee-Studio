// Compose's renderer (renderMessage) against the Python renderer (studio/marquee_studio/render.py render_message555,
// test/make_message_fixtures.py): every frame of every case identical. Scroll and static, flash, money values,
// effects (the wipe, effect slots, a transparent background, an animation that is not a slot), outlines, native emoji.
import { test } from "node:test"
import assert from "node:assert/strict"
import { existsSync, readdirSync, readFileSync } from "node:fs"
import { join } from "node:path"
import { renderMessage, unpackEmojiSheet, type FwFont } from "../src/index.ts"
import { fixtures, loadFonts, ROOT, sha } from "./load.ts"

const fx = fixtures("message.fixtures.json")
const names = readdirSync(join(ROOT, "studio", "content", "fonts")).filter((n) => /^fonte\d\d\.png$/.test(n)).sort()
const fonts = new Map<string, FwFont>((await loadFonts(names)).map((f, i) => [names[i], f]))
const anims = join(ROOT, "studio", "content", "animations")
const slots = new Map<number, Uint8Array>()
for (const n of readdirSync(anims)) {
  const m = /^effect(\d\d)\.flc$/.exec(n)
  if (m) slots.set(+m[1], new Uint8Array(readFileSync(join(anims, n))))
}
// logo_e.flc is the non-slot case: a test input only, kept with the dump's extracted files and out of the Studio's content
const animFile = (n: string) => (existsSync(join(anims, n)) ? join(anims, n) : join(ROOT, "analysis", "extracted", n))
const extra = new Map<string, Uint8Array>((fx.anims as string[]).map((n) => [n, new Uint8Array(readFileSync(animFile(n)))]))
const sheet = await unpackEmojiSheet(new Uint8Array(readFileSync(new URL("./emoji16.bin", import.meta.url))))

for (const c of fx.cases) {
  test(c.name, () => {
    const frames = renderMessage(c.spec, c.W, c.H, { fonts, slots, anims: extra, sheet })
    assert.equal(frames.length, c.sha256.length, "frame count")
    const first = frames.findIndex((f, i) => sha(f) !== c.sha256[i])
    assert.equal(first, -1, `frame ${first} differs`)
  })
}

// A wave must ripple at every scroll speed. Its phase once came from the panel column, so at 1 px a step the scroll
// cancelled it and the letters froze in one bent shape.
test("a wave animates at every scroll speed, with or without a fill", () => {
  const enc = (s: string) => new TextEncoder().encode(s)
  const font = "fonte04.png", W = 200, H = 16, pre = "You're doing a ", mid = "great job"
  const ft = fonts.get(font)!
  const u0 = ft.textWidth(enc(pre)), u1 = ft.textWidth(enc(pre + mid))
  for (const fill of ["", "\x1ffC000"]) for (const rate of [1, 2, 3, 4]) {
    const text = pre + "\x1fm1" + fill + mid + "\x1fm0" + (fill && "\x1ff-") + "!"
    const frames = renderMessage({ text, font, mode: "scroll", text_rate: rate, colour: "Gold", bg_colour: "Off", outline: "Off",
      native_emoji: true, emoji_size: 16, flash_on_ms: 0, flash_off_ms: 0 } as never, W, H, { fonts, slots, anims: new Map(), sheet })
    const shapes = new Set<string>()
    frames.forEach((f, k) => {
      const x0 = W - k * rate
      if (x0 + u0 < 0 || x0 + u1 >= W) return
      let s = ""
      for (let u = u0; u < u1; u++) for (let y = 0; y < H; y++) s += f.d[y * W + x0 + u] ? "1" : "0"
      shapes.add(s)
    })
    assert.ok(shapes.size > 4, `rate ${rate}${fill ? " with a fill" : ""}: ${shapes.size} shapes`)
  }
})

// An inline animated emoji (an imported GIF) plays in Compose's preview, scrolling and still: every frame in its turn.
test("an inline animated emoji plays in the preview", () => {
  // the sheet's first cell given two frames: itself and its colours inverted, 100 ms each
  const cell = 0, f0 = sheet.frames[cell][0], f1 = f0.map((v) => (v < 0 ? v : v ^ 0x7fff))
  const anim = { ...sheet, frames: sheet.frames.map((f, i) => (i === cell ? [f0, f1 as Int32Array] : f)), ms: sheet.ms.map((m, i) => (i === cell ? 100 : m)) }
  const text = new TextDecoder().decode(sheet.keys[cell])
  for (const mode of ["static", "scroll"]) {
    const frames = renderMessage({ text: "A" + text + "B", font: "fonte04.png", mode, text_rate: 1, colour: "Gold", bg_colour: "Off", outline: "Off",
      native_emoji: true, emoji_size: 16, flash_on_ms: 0, flash_off_ms: 0 } as never, 200, 16, { fonts, slots, anims: new Map(), sheet: anim })
    const has = (fr: Int32Array) => frames.some((f) => [...fr].filter((v) => v >= 0).every((v) => f.d.includes(v)))
    assert.ok(has(f0) && has(f1 as Int32Array), `${mode}: both frames of the cell are shown, in ${frames.length} frames`)
  }
})

// A letter's own fill stops at its own columns, as the firmware's span does (its width and its spacer column): the
// character after it keeps the message's fill.
test("a letter's fill does not bleed into the next character", () => {
  const enc = (s: string) => new TextEncoder().encode(s)
  const font = "fonte04.png", W = 200, H = 16, ft = fonts.get(font)!
  const draw = (text: string) => renderMessage({ text, font, mode: "static", colour: "Gold", bg_colour: "Off", outline: "Off",
    native_emoji: true, emoji_size: 16, flash_on_ms: 0, flash_off_ms: 0 } as never, W, H, { fonts, slots, anims: new Map(), sheet })[0]
  const plain = draw("job!"), filled = draw("\x1ffC00Djob\x1ff-!")
  const left = (W - ft.textWidth(enc("job!"))) >> 1, from = left + ft.textWidth(enc("job"))
  for (let y = 0; y < H; y++)
    for (let x = from; x < W; x++)
      assert.equal(filled.d[y * W + x], plain.d[y * W + x], `column ${x - from} of the "!", row ${y}`)
})

/** A one-frame 16-bit FLIC where every pixel is its own colour: red the column, green the row. */
function stillTexture(w: number, h: number): Uint8Array {
  const body = 6 + 2 * w * h, frame = 16 + body, out = new DataView(new ArrayBuffer(128 + frame))
  out.setUint32(0, 128 + frame, true); out.setUint16(4, 0xaf12, true); out.setUint16(6, 1, true)
  out.setUint16(8, w, true); out.setUint16(10, h, true); out.setUint16(12, 16, true)
  out.setUint32(0x10, 200, true); out.setUint32(0x50, 128, true)
  out.setUint32(128, frame, true); out.setUint16(132, 0xf1fa, true); out.setUint16(134, 1, true)
  out.setUint32(144, body, true); out.setUint16(148, 16, true)
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) out.setUint16(150 + 2 * (y * w + x), (x << 10) | (y << 5) | 0x1f, true)
  return new Uint8Array(out.buffer)
}

// A moving letter carries its fill: at any step of a wave or a bounce, a letter's pixels show the same part of the
// texture. A still texture (animation slot 40).
test("a moving letter's fill goes with it", () => {
  const W = 200, H = 16
  const texSlots = new Map([[40, stillTexture(32, 16)]])
  const enc = (s: string) => new TextEncoder().encode(s), font = "fonte04.png", ft = fonts.get(font)!
  const u0 = ft.textWidth(enc("AB")), u1 = ft.textWidth(enc("ABHELLO")), tw = ft.textWidth(enc("ABHELLO!"))
  for (const [fill, motion] of [["\x1ffC028", "\x1fm1"], ["\x1ffC028", "\x1fm2"]]) for (const [mode, rate] of [["static", 1], ["scroll", 2]] as const) {
    const frames = renderMessage({ text: "AB" + fill + motion + "HELLO\x1fm0\x1ff-!", font, mode, text_rate: rate, colour: "Gold", bg_colour: "Off", outline: "Off",
      native_emoji: true, emoji_size: 16, flash_on_ms: 0, flash_off_ms: 0 } as never, W, H, { fonts, slots: texSlots, anims: new Map(), sheet })
    // a column's colours in any one frame, and over all frames: the same, if the texture moves with the letter (a
    // pixel can be clipped at the panel's edge, but no new colour can appear)
    const seen = new Map<number, Set<number>>(), widest = new Map<number, number>()
    frames.forEach((f, k) => {
      const x0 = mode === "static" ? (W - tw) >> 1 : W - k * rate
      if (x0 + u0 < 0 || x0 + u1 >= W) return
      for (let u = u0; u < u1; u++) {
        const here = new Set<number>()
        for (let y = 0; y < H; y++) if (f.d[y * W + x0 + u]) here.add(f.d[y * W + x0 + u])
        if (!seen.has(u)) seen.set(u, new Set())
        here.forEach((v) => seen.get(u)!.add(v))
        widest.set(u, Math.max(widest.get(u) ?? 0, here.size))
      }
    })
    for (const [u, all] of seen)
      assert.equal(all.size, widest.get(u), `${JSON.stringify(motion)} ${mode}, column ${u}: ${all.size} colours over time, at most ${widest.get(u)} at once`)
  }
})
