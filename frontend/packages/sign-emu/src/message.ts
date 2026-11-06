// A Compose message as the sign draws a pool's text item, ported from studio/marquee_studio/render.py
// (render_message555 / message_layout) with amount.py, effects.py, flash.py and native_emoji.py: the text (and a money
// Value) scrolled at text_rate px per frame or centred, flashed, drawn through draw_text_item (the background over
// the window, the outline, the letters), the native emoji on top. Checked frame for
// frame against the Python renderer (test/make_message_fixtures.py).

import { half, img, type Img } from "./image.ts"
import type { FwFont } from "./font.ts"
import { Effects, flcSpeedToHold, outlineBlit, OUTLINE_COLOURS, OUTLINE_STYLES, colourIndexToRgb555 } from "./effects.ts"
import { Flic } from "./flic.ts"

export const PALETTE: Record<string, number> = {
  Red: 0x7c00, Green: 0x03e0, Yellow: 0x7fe0, Amber: 0x7e80, Orange: 0x7d00, "Dim Red": 0x3c00, "Dim Green": 0x01e0,
  "Dim Yellow": 0x3de0, Gold: 0x7ea0, White: 0x7fff, Blue: 0x001f, Cyan: 0x03ff, Magenta: 0x7c1f, Off: 0x0000,
}
export const SPECIAL_EFFECTS: Record<string, number> = { Transparent: 0x8000, "Red/green/yellow wipe": 0x8001 }

/** effect_code: a colour name, a special, effectNN.flc, or hex; null when not expressible. */
export function effectCode(sel: unknown): number | null {
  if (!sel) return 0
  if (typeof sel === "number") return sel & 0xffff
  if (typeof sel !== "string") return null
  if (sel in PALETTE) return PALETTE[sel]
  if (sel in SPECIAL_EFFECTS) return SPECIAL_EFFECTS[sel]
  const m = /^effect(\d+)\.flc$/.exec(sel)
  if (m && +m[1] < 100) return 0xc000 | +m[1]
  if (sel.toLowerCase().startsWith("0x")) {
    const v = parseInt(sel, 16)
    return Number.isNaN(v) ? null : v & 0xffff
  }
  return null
}

/** (style, colour index) of the spec's outline; another colour name takes the nearest of the 8. */
export function outlineOf(spec: Spec): [number, number] {
  const col = spec.outline ?? "Off"
  if (col === null || col === "" || col === "Off" || col === "None") return [0, 0]
  let idx: number
  if (typeof col === "number") idx = col
  else if (OUTLINE_COLOURS.includes(col)) idx = OUTLINE_COLOURS.indexOf(col)
  else {
    const v = PALETTE[col] ?? 0
    const rgb = (c: number) => [(c >> 10) & 31, (c >> 5) & 31, c & 31]
    const d = (i: number) => rgb(v).reduce((s, p, k) => s + (p - rgb(colourIndexToRgb555(i))[k]) ** 2, 0)
    idx = 0
    for (let i = 1; i < 8; i++) if (d(i) < d(idx)) idx = i
  }
  const st = spec.outline_style ?? "Thick"
  return [OUTLINE_STYLES.includes(String(st)) ? OUTLINE_STYLES.indexOf(String(st)) : Math.trunc(Number(st)), idx]
}

/** (on, off) in panel frames */
/** flash.py flash_warnings: problems with a level's flash times (milliseconds), as text. */
export function flashWarnings(onMs: number, offMs: number): string[] {
  const w: string[] = []
  const ok = (v: number) => 25 <= v && v <= 6375
  if ((onMs && !ok(onMs)) || (offMs && !ok(offMs))) w.push("flash times must be 25..6375 ms (the sign's menu range)")
  const cycle = onMs + Math.max(offMs, 25)
  if (onMs && cycle < 334)
    w.push(`flashes ${(1000 / cycle).toFixed(1)} times a second: more than 3 per second can trigger seizures (WCAG 2.3.1); use a cycle of 334 ms or more`)
  return w
}

export function flashFrames(spec: Spec): [number, number] {
  if ("flash_on_ms" in spec || "flash_off_ms" in spec) {
    const on = Math.trunc(Number(spec.flash_on_ms) || 0), off = Math.trunc(Number(spec.flash_off_ms) || 0)
    return [on ? Math.floor(on / 25) : 0, off ? Math.floor(off / 25) : 0]
  }
  return [Math.trunc(Number(spec.flash_on) || 0), Math.trunc(Number(spec.flash_off) || 0)]
}

// ---- money (amount_format, ELITE_RGB 0x479B4) ----
export function amountFormat(value: number, decimals = 2, mode = 1, dec = ".", thou = ",", prefix = "$", suffix = ""): number[] {
  let pre = latin1(prefix)
  if ((mode === 0 || mode === 2) && pre.length === 1 && pre[0] === 0x24) pre = [0x1c]
  const d = latin1(String(value >>> 0).padStart(decimals + 1, "0"))
  const n = d.length
  if (mode === 2 || mode === 3) for (let i = n - decimals; i < n; i++) d[i] -= 0x30
  if (mode === 0 || mode === 2) for (let i = 0; i < n - decimals; i++) d[i] -= 0x20
  if (mode === 0) for (let i = n - decimals; i < n; i++) d[i] -= 0x20
  let k = 0, ln = n
  if (decimals) {
    d.splice(ln - decimals, 0, dec.charCodeAt(0))
    k = decimals + 1
    ln++
  }
  if (thou && thou !== "\0") {
    let u = k + 3
    while (u < ln) {
      d.splice(ln - u, 0, thou.charCodeAt(0))
      u += 4
      ln++
    }
  }
  return [...pre, ...d.map((x) => x & 0xff), ...latin1(suffix)]
}

const latin1 = (s: string) => Array.from(s, (c) => { const v = c.codePointAt(0)!; return v < 256 ? v : 0x3f })

/** Python's round(): half to even */
function roundHalfEven(x: number): number {
  const f = Math.floor(x), r = x - f
  if (r > 0.5) return f + 1
  if (r < 0.5) return f
  return f % 2 === 0 ? f : f + 1
}

export interface AmountOptions { decimals?: number; mode?: number; dec?: string; thou?: string; prefix?: string; suffix?: string }

/** A Value field as the sign shows it, as a string of font codes. */
export function amountText(value: string | number, opts: AmountOptions = {}): string {
  const o = { decimals: 2, mode: 1, dec: ".", thou: ",", prefix: "$", suffix: "", ...opts }
  const d = Math.trunc(Number(o.decimals)), m = Math.trunc(Number(o.mode))
  let s: string | number = value
  if (typeof value === "string") {
    s = value.trim().replaceAll(o.thou || "\0", "")
    s = o.prefix ? s.replaceAll(o.prefix, "").replaceAll(o.dec, ".") : s.replaceAll(o.dec, ".")
  }
  const minor = roundHalfEven(Number(s) * 10 ** d)
  if (!(minor >= 0 && minor < 2 ** 32)) throw new Error("amount out of range (0 .. 4294967295 in minor units)")
  return String.fromCharCode(...amountFormat(minor, d, m, o.dec, o.thou, o.prefix, o.suffix))
}

export * from "./native.ts"
import { fxApply, fxOffset, nativeEncode, nativeLayout, nativeParse, nativeTop, spanOn, type EmojiSheet, type FxState, type Glyph } from "./native.ts"

// ---- the message ----
export interface Spec {
  text?: string
  value?: string | number | null
  amount?: AmountOptions
  font?: string
  native_emoji?: boolean
  emoji_size?: number
  foreground?: string
  colour?: string
  background?: string
  bg_colour?: string
  outline?: string | number | null
  outline_style?: string | number
  frame0?: number
  flash_on_ms?: number
  flash_off_ms?: number
  flash_on?: number
  flash_off?: number
  mode?: string
  text_rate?: number
}

export interface MessageAssets {
  /** the sign's fonts by name (fonteNN.png) */
  fonts: Map<string, FwFont>
  /** effectNN.flc by slot */
  slots: Map<number, Uint8Array>
  /** other animations by name (a background that is not an effect slot is previewed in a free slot) */
  anims?: Map<string, Uint8Array>
  /** the native emoji sheet at the emoji size */
  sheet?: EmojiSheet
}

/** render_message555: the message's frames (RGB555), one per 40 fps panel frame. */
export function renderMessage(spec: Spec, W: number, H: number, A: MessageAssets): Img[] {
  let text = spec.text ?? ""
  if (spec.value !== undefined && spec.value !== null && spec.value !== "") text = amountText(spec.value, spec.amount ?? {}) + " " + text
  const fontName = spec.font ?? "fonte01.png"
  const ft = A.fonts.get(fontName)
  if (!ft) throw new Error(`no font ${fontName}`)
  const native = !!spec.native_emoji
  const esize = Math.trunc(Number(spec.emoji_size) || 16)
  let data: Uint8Array, glyphs: Glyph[] | null = null
  if (native) {
    if (!A.sheet) throw new Error("no emoji sheet")
    data = nativeEncode(text, A.sheet)
    glyphs = nativeParse(data, A.sheet)
  } else {
    data = Uint8Array.from(Array.from(text).map((c) => c.codePointAt(0)!).filter((v) => v < 256))
  }
  const slots = new Map(A.slots)
  const codes: number[] = []
  for (const [key, solid, dflt] of [["foreground", "colour", "Amber"], ["background", "bg_colour", "Off"]] as const) {
    const sel = spec[key] || (spec[solid] ?? dflt)
    let c = effectCode(sel)
    if (c === null && typeof sel === "string" && sel.endsWith(".flc")) {
      let n = 99
      while (n >= 0 && slots.has(n)) n--
      const bytes = A.anims?.get(sel)
      if (bytes) slots.set(n, bytes)
      c = 0xc000 | n
    }
    codes.push(c === null ? 0x7fe0 : c)
  }
  const word = ((codes[0] << 16) | codes[1]) >>> 0
  const [style, colour] = outlineOf(spec)
  const placed = glyphs ? nativeLayout(ft, glyphs, esize) : null
  // per-character effects (in-text commands, 0x1F): the effects of each placed glyph, in order
  const efx: FxState[] = []
  if (glyphs) {
    const e: FxState = { kind: 0, hasFill: false, fill: 0, on: 0, off: 0 }
    for (const g of glyphs) {
      if (g.kind === "x") fxApply(e, g)
      else efx.push({ ...e })
    }
  }
  const has = (e: FxState | undefined) => !!e && (e.kind !== 0 || e.hasFill || e.on !== 0)
  const moving = efx.some(has)
  const tw = placed ? placed[1] : ft.textWidth(data)
  const ty = half(H - ft.ch)
  const frame0 = (Math.trunc(Number(spec.frame0 ?? 1000)) >>> 0)
  const [flashOn, flashOff] = flashFrames(spec)
  const flashed = (step: number) => flashOn <= 0 || step % (flashOn + Math.max(1, flashOff)) < flashOn
  let pos: ([number, number] | null)[]
  if ((spec.mode ?? "scroll") === "static") {
    const periods = [1]
    for (const c of codes) {
      if (c === 0x8001) periods.push(12 * H)
      else if (c === 0x8002) periods.push(24)
      else if (c === 0xbffe || c === 0xbfff) periods.push(48)
      else if (c >= 0xc000 && slots.has(c & 0x3fff)) {
        const fl = new Flic(slots.get(c & 0x3fff)!)
        periods.push(Math.max(1, fl.frames) * (flcSpeedToHold(fl.speedMs) + 1))
      }
    }
    if (flashOn > 0) periods.push(flashOn + Math.max(1, flashOff))
    for (const g of glyphs ?? [])                         // animated emoji: long enough to show every frame
      if (g.kind === "e" && A.sheet && A.sheet.ms[g.cell]) periods.push(Math.max(1, Math.round((A.sheet.frames[g.cell].length * A.sheet.ms[g.cell]) / 25)))
    if (moving) periods.push(96)                          // wave and bounce repeat every 32 steps, blink every 24
    const gcd = (a: number, b: number): number => (b ? gcd(b, a % b) : a)
    let n = 1
    for (const p of periods) n = (n * p) / gcd(n, p)
    n = n <= 480 ? n : Math.max(...periods)
    const x = half(W - tw)
    pos = Array.from({ length: n }, () => [x, ty] as [number, number])
  } else {
    const rate = Math.max(1, Math.trunc(Number(spec.text_rate ?? 4)))
    pos = []
    for (let shift = 0; shift < tw + W; shift += rate) pos.push([W - shift, ty])
  }
  pos = pos.map((p, k) => (flashed(k) ? p : null))
  const fx = new Effects(slots, 0)
  const out: Img[] = []
  pos.forEach((p, k) => {
    fx.frameCount = (frame0 + k) >>> 0
    const frame = img(W, H), mask = img(W, H)
    // a glyph with effects: its columns displaced as the firmware displaces them (native.ts fxOffset): columns
    // counted from the first effect letter, so they move with the text, step k
    const fxOrigin = p && placed ? p[0] + (placed[0].find(({ w }, ci) => w > 0 && has(efx[ci]))?.x ?? 0) : 0
    const fills = new Map<number, number>()             // panel column -> the letters' own fill word, while on
    const carried = new Map<number, [number, number]>() // panel column of a letter -> where its fill's texture comes from
    const shifted = (e: FxState, ci: number, x0: number, w: number, span: number, draw: (dst: Img, x: number) => void) => {
      const tmp = img(w + 2, H)
      draw(tmp, 0)
      if (e.hasFill && spanOn(e, k)) for (let cx = 0; cx < span; cx++) fills.set(x0 + cx, e.fill)
      for (let cx = 0; cx < w + 2; cx++) {
        const [dx, dy, vis] = fxOffset(e, x0 + cx - fxOrigin, ci, k)
        if (cx < span) carried.set(x0 + cx, [x0 + cx - dx - fxOrigin, -dy])
        if (!vis) continue
        for (let y = 0; y < H; y++) {
          const sx = cx - dx, sy = y - dy, X = x0 + cx
          if (sx < 0 || sx >= w + 2 || sy < 0 || sy >= H || X < 0 || X >= W) continue
          const v = tmp.d[sy * tmp.w + sx]
          if (v) mask.d[y * W + X] = v
        }
      }
    }
    if (p) {
      if (placed) {
        placed[0].forEach(({ g, x, w }, ci) => {
          if (g.kind !== "c") return
          if (moving && has(efx[ci])) shifted(efx[ci], ci, p[0] + x, w, w + (ft.prop[g.code] ? 1 : 0), (dst, dx) => ft.draw(dst, dx, p[1], [g.code], 0x7fff, 0))
          else ft.draw(mask, p[0] + x, p[1], [g.code], 0x7fff, 0)
        })
      } else ft.draw(mask, p[0], p[1], data, 0x7fff, 0)
    }
    if (!fills.size && !carried.size) drawTextItem(fx, frame, mask, word, [0, 0, W, H], style, colour)
    else {                                                // letters with their own fill: the letters in runs
      fx.blitEffect(img(W, H), frame, word, [0, 0, W, H], [0, 0])
      outlineBlit(mask, [0, 0, W, H], frame, [0, 0], style, colour, [0, 0, W, H])
      const wordAt = (x: number) => (fills.has(x) ? (((fills.get(x)! << 16) | 0x8000) >>> 0) : ((word & 0xffff0000) | 0x8000) >>> 0)
      for (let x = 0; x < W; ) {
        let x1 = x + 1
        const c0 = carried.get(x)
        // a letter's texture runs with its columns; a change of displacement starts a new run
        while (x1 < W && wordAt(x1) === wordAt(x) && carried.get(x1)?.[1] === c0?.[1] && (carried.get(x1)?.[0] ?? 0) - (c0?.[0] ?? 0) === (c0 ? x1 - x : 0)) x1++
        if (c0) fx.blitTex(mask, frame, wordAt(x), [x, 0, x1 - x, H], [x, 0], c0)
        else fx.blitEffect(mask, frame, wordAt(x), [x, 0, x1 - x, H], [x, 0])
        x = x1
      }
    }
    if (p && placed && A.sheet) {
      placed[0].forEach(({ g, x }, ci) => {
        if (g.kind !== "e") return
        // an animated cell at its frame for this step, by the firmware's rule (native.ts animPut)
        const frs = A.sheet!.frames[g.cell], ms = A.sheet!.ms[g.cell]
        const cell = frs[ms ? Math.floor((((frame0 + k) >>> 0) * 25 >>> 0) / ms) % frs.length : 0], t = nativeTop(ft.ch, esize)
        for (let n = 0; n < cell.length; n++) {
          if (cell[n] < 0) continue
          let X = p[0] + x + (n % esize), Y = p[1] + t + Math.floor(n / esize)
          if (moving && has(efx[ci])) {
            const [dx, dy, vis] = fxOffset(efx[ci], X - fxOrigin, ci, k)
            if (!vis) continue
            X += dx, Y += dy
          }
          if (X >= 0 && X < W && Y >= 0 && Y < H) frame.d[Y * W + X] = cell[n]   // opaque RGB555
        }
      })
    }
    out.push(frame)
  })
  return out
}


/** draw_text_item: the background over the window, the outline around the text mask, then the letters. */
export function drawTextItem(fx: Effects, frame: Img, mask: Img, word: number, window: [number, number, number, number],
                             style = 0, colour = 0) {
  fx.blitEffect(img(mask.w, mask.h), frame, word, [...window], [window[0], window[1]])
  outlineBlit(mask, [...window], frame, [window[0], window[1]], style, colour, [0, 0, frame.w, frame.h])
  fx.blitEffect(mask, frame, ((word & 0xffff0000) | 0x8000) >>> 0, [...window], [window[0], window[1]])
}
