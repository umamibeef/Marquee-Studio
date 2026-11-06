// Native emoji (firmware/ext/native_emoji.c, studio native_emoji.py): the sign's emoji sheet, the UTF-8 bytes that
// name its cells, and the firmware's emoji-aware text_width / text_draw_proportional / glyph_blit_effect hooks.
import type { FwFont } from "./font.ts"
import { vline } from "./font.ts"
import type { Img } from "./image.ts"

export interface EmojiSheet {
  size: number
  /** the map entries as UTF-8 bytes, in cell order */
  keys: Uint8Array[]
  /** the emoji (without U+FE0F) in cell order */
  emoji: string[]
  /** per cell: size * size RGB555 values, or -1 where transparent (frame 0) */
  cells: Int32Array[]
  /** per cell: its frames (a still has one) */
  frames: Int32Array[][]
  /** per cell: ms per frame (0 for a still) */
  ms: number[]
}

/** emoji.bin: ">IHHHHII" header ("EMJ2", size, count, colours, frames, map length, unpacked length), then
 *  zlib(palette, map, cell table (count x >HHH: first frame, frames, ms), pixels). "EMJ1": no table, a frame a
 *  cell. */
export async function unpackEmojiSheet(bin: Uint8Array): Promise<EmojiSheet> {
  const dv = new DataView(bin.buffer, bin.byteOffset, bin.byteLength)
  const magic = dv.getUint32(0)
  if (magic !== 0x454d4a31 && magic !== 0x454d4a32) throw new Error("not an emoji.bin")
  const size = dv.getUint16(4), count = dv.getUint16(6), colours = dv.getUint16(8), mapLen = dv.getUint32(12)
  const ds = new DecompressionStream("deflate")
  const raw = new Uint8Array(await new Response(new Blob([bin.subarray(20) as BlobPart]).stream().pipeThrough(ds)).arrayBuffer())
  const pal: number[] = []
  for (let i = 0; i < colours; i++) pal.push((raw[2 * i] << 8) | raw[2 * i + 1])
  const map = new TextDecoder().decode(raw.subarray(colours * 2, colours * 2 + mapLen))
  const emoji = map.split("\n").slice(0, count)
  let at = colours * 2 + mapLen
  const table = emoji.map((_, n) => magic === 0x454d4a31 ? [n, 1, 0] : [0, 2, 4].map((o) => (raw[at + 6 * n + o] << 8) | raw[at + 6 * n + o + 1]))
  if (magic !== 0x454d4a31) at += 6 * count
  const px = raw.subarray(at), n2 = size * size
  const frame = (k: number) => Int32Array.from(px.subarray(k * n2, (k + 1) * n2), (v) => (v ? pal[v] : -1))
  const frames = table.map(([f0, nf]) => Array.from({ length: nf }, (_, k) => frame(f0 + k)))
  return { size, keys: emoji.map((e) => new TextEncoder().encode(e)), emoji, cells: frames.map((f) => f[0]), frames,
           ms: table.map((t) => t[2]) }
}

const stripVS = (s: string) => s.replaceAll("️", "")

/** encode(): the set's emoji as UTF-8 (longest first, U+FE0F dropped), everything else Latin-1 ('?' otherwise). */
export function nativeEncode(text: string, sheet: EmojiSheet): Uint8Array {
  const keys = [...new Set(sheet.emoji)].sort((a, b) => b.length - a.length)
  const out: number[] = []
  const cps = Array.from(text)
  let i = 0
  while (i < cps.length) {
    if (cps[i] === "️") { i++; continue }
    const rest = stripVS(cps.slice(i, i + 12).join(""))
    const hit = keys.find((k) => rest.startsWith(k))
    if (hit) {
      out.push(...new TextEncoder().encode(hit))
      let n = 0, j = i
      const want = Array.from(hit).length
      while (n < want) { if (cps[j] !== "️") n++; j++ }
      i = j
      continue
    }
    const v = cps[i].codePointAt(0)!
    out.push(v < 256 ? v : 0x3f)
    i++
  }
  return Uint8Array.from(out)
}

/** a glyph of a text: a font character, an emoji, or an in-text effect command (0x1F, no room: native_emoji.c) */
export type Glyph = { kind: "c"; code: number } | { kind: "e"; cell: number } | { kind: "x"; cmd: number; val: number }

const hexv = (c: number) => (c >= 48 && c <= 57 ? c - 48 : c >= 65 && c <= 70 ? c - 55 : c >= 97 && c <= 102 ? c - 87 : -1)
/** fx_cmd: an effect command at data[i] (0x1F): [length, command ('m' 'f' 'o' as char codes, 0 for a lone 0x1F),
 *  value (a fill reset: 0x10000)]; null when data[i] is not 0x1F */
export function fxCommand(data: ArrayLike<number>, i: number): [number, number, number] | null {
  const n = data.length - i
  if (n < 1 || data[i] !== 0x1f) return null
  const c1 = n >= 2 ? data[i + 1] : 0
  if (n >= 3 && c1 === 109 && data[i + 2] >= 48 && data[i + 2] <= 52) return [3, 109, data[i + 2] - 48]
  if (n >= 3 && c1 === 102 && data[i + 2] === 45) return [3, 102, 0x10000]
  if (n >= 6 && (c1 === 102 || c1 === 111)) {
    let w = 0
    for (let k = 2; k < 6; k++) {
      const v = hexv(data[i + k])
      if (v < 0) return [1, 0, 0]
      w = (w << 4) | v
    }
    return [6, c1, w]
  }
  return [1, 0, 0]
}

/** parse(): the glyphs of text bytes as the firmware reads them. */
export function nativeParse(data: Uint8Array, sheet: EmojiSheet): Glyph[] {
  const out: Glyph[] = []
  const starts = (k: Uint8Array, at: number) => k.length <= data.length - at && k.every((b, j) => data[at + j] === b)
  for (let i = 0; i < data.length; ) {
    const fc = fxCommand(data, i)
    if (fc) { out.push({ kind: "x", cmd: fc[1], val: fc[2] }); i += fc[0]; continue }
    let best = 0, cell = -1
    if (data[i] >= 0xc2) {
      sheet.keys.forEach((k, n) => { if (k.length > best && starts(k, i)) { best = k.length; cell = n } })
      if (best && data[i + best] === 0xef && data[i + best + 1] === 0xb8 && data[i + best + 2] === 0x8f) best += 3
    }
    if (best) { out.push({ kind: "e", cell }); i += best }
    else { out.push({ kind: "c", code: data[i] }); i++ }
  }
  return out
}

/** layout(): [(glyph, x, width)] and the total width (an emoji is a proportional glyph a cell wide). */
export function nativeLayout(ft: FwFont, glyphs: Glyph[], size: number): [{ g: Glyph; x: number; w: number }[], number] {
  const out: { g: Glyph; x: number; w: number }[] = []
  let x = 0, prev = 1
  for (const g of glyphs) {
    if (g.kind === "x") continue                         // an effect command takes no room
    const [w, p] = g.kind === "e" ? [size, 1] : [ft.width[g.code], ft.prop[g.code]]
    if (prev === 0 && p) x++
    out.push({ g, x, w })
    x += w + (p ? 1 : 0)
    prev = p
  }
  return [out, x]
}

/** emoji_draw's top row, relative to an h-pixel line (centred, C division) */
export const nativeTop = (h: number, size: number) => Math.trunc((h - size) / 2)


/** emoji_draw's tag: an emoji pixel in a message line's text image (TEXT_IMG_W wide) carries bit 15, so that
 *  glyph_blit_effect puts its own colour back over the line's fill. */
export const EMOJI_TAG = 0x8000
const TEXT_IMG_W = 0x1414

/** ext_text_width: the width of text bytes with the sheet's emoji (each a proportional glyph a cell wide). */
export function nativeTextWidth(f: FwFont, data: Uint8Array, sheet: EmojiSheet | undefined): number {
  const g = nativeParse(data, sheet ?? NO_SHEET)
  if (!g.some((x) => x.kind !== "c")) return f.textWidth(data)
  if (!sheet) return nativeLayout(f, g, 0)[1]
  return nativeLayout(f, g, sheet.size)[1]
}

/** ext_text_draw_proportional (draw_prop with colours): fg/bg letters, emoji cells 1:1 centred on the line. */
export function nativeDraw(f: FwFont, im: Img, x: number, y: number, data: Uint8Array, fg: number, bg: number,
                           sheet: EmojiSheet | undefined): number {
  const glyphs = nativeParse(data, sheet ?? NO_SHEET)
  if (!glyphs.some((g) => g.kind !== "c")) {
    motionReset(im)
    return f.draw(im, x, y, data, fg, bg)
  }
  const x0 = x, s = sheet?.size ?? 0, tag = im.w === TEXT_IMG_W ? EMOJI_TAG : 0
  const spans = im.w === TEXT_IMG_W ? motionTable(im) : null
  let prev = 1, ci = 0
  const e: FxState = { kind: 0, hasFill: false, fill: 0, on: 0, off: 0 }
  for (const g of glyphs) {
    if (g.kind === "x") { fxApply(e, g); continue }
    const [w, p] = g.kind === "e" ? [s, 1] : [f.width[g.code], f.prop[g.code]]
    if (!prev && p) {
      vline(im, x, y, f.ch, 0)
      x++
    }
    const sx0 = x
    if (g.kind === "e" && sheet) {
      const top = y + nativeTop(f.ch, s), cell = sheet.cells[g.cell]
      if (tag && sheet.frames[g.cell].length > 1) noteAnim(im, sheet, x, top, g.cell)
      for (let dy = 0; dy < s; dy++) {
        const py = top + dy
        if (py < 0 || py >= im.h) continue
        for (let dx = 0; dx < s; dx++) {
          const px = x + dx, v = cell[dy * s + dx]
          if (v >= 0 && px >= 0 && px < im.w) im.d[py * im.w + px] = v | tag
        }
      }
    } else if (g.kind === "c") f.glyph(im, x, y, g.code, fg, bg)
    x += w
    if (p) {
      vline(im, x, y, f.ch, bg)
      x++
    }
    if (spans && (e.kind || e.hasFill || e.on) && x > sx0 && spans.length < MSPANS) spans.push({ x0: sx0, x1: x, ci: ci & 0xff, ...e })
    ci++
    prev = p
  }
  return (x - x0) & 0xffff
}

// ---- per-character motion (native_emoji.c: motion markers, spans, motion_blit) ----
const NO_SHEET: EmojiSheet = { size: 0, keys: [], emoji: [], cells: [], frames: [], ms: [] }
const MSPANS = 96
/** the effects in force while a text is drawn (its commands so far): native_emoji.c struct fxstate */
export interface FxState { kind: number; hasFill: boolean; fill: number; on: number; off: number }
interface MotionSpan extends FxState { x0: number; x1: number; ci: number }
export function fxApply(e: FxState, g: { cmd: number; val: number }) {
  if (g.cmd === 109) e.kind = g.val
  else if (g.cmd === 102) { e.hasFill = g.val !== 0x10000; e.fill = g.val & 0xffff }
  else if (g.cmd === 111) { e.on = g.val >> 8; e.off = g.val & 0xff }
}
/** span_on: the effects run at step t (always without an on/off time) */
export function spanOn(e: { on: number; off: number }, t: number): boolean {
  const period = (e.on + e.off) * 4
  return !e.on || !period || (t >>> 0) % period < e.on * 4
}
/** the displacement and visibility of column sx of glyph ci with these effects at step t (motion_blit's rule) */
export function fxOffset(e: FxState, sx: number, ci: number, t: number): [number, number, boolean] {
  const on = spanOn(e, t)
  if (e.kind === 4 && e.on) return [0, 0, on]               // blink with an on/off time: that is its rhythm
  return on ? motionOffset(e.kind, sx, ci, t) : [0, 0, true]
}
const motions = new WeakMap<Uint16Array, MotionSpan[]>()
function motionTable(im: Img): MotionSpan[] {
  const t: MotionSpan[] = []
  motions.set(im.d, t)
  return t
}
/** a new text drawn into im (no effects): its old spans no longer apply */
export function motionReset(im: Img) {
  motions.delete(im.d)
}

/** ext_image_blit: src copied into dst at x (the whole src, as a slide does): dst gets src's spans moved by x,
 *  clipped to src's extent there and to dst; none when src has none */
export function motionCopy(src: Img, dst: Img, x: number) {
  const from = motions.get(src.d)
  if (!from || !from.length) { motions.delete(dst.d); return }
  const to: MotionSpan[] = []
  for (const sp of from) {
    if (sp.x1 <= 0 || sp.x0 >= src.w) continue
    let x0 = sp.x0 + x, x1 = sp.x1 + x
    x0 = Math.max(x0, x, 0); x1 = Math.min(x1, x + src.w, dst.w)
    if (x1 > x0 && to.length < MSPANS) to.push({ ...sp, x0, x1 })
  }
  motions.set(dst.d, to)
}
const WAVE = [0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, -1, -1,
  -1, -1, -1, -1, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -1, -1, -1, -1, -1, -1, 0, 0]
const HOP = [0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 1, 1, 1, 1, 0]
/** motion kinds, as the markers number them */
export const MOTION = { none: 0, wave: 1, bounce: 2, shake: 3, blink: 4 } as const

/** motion_offset: [dx, dy, visible] of column sx of glyph ci with this motion at step t */
export function motionOffset(kind: number, sx: number, ci: number, t: number): [number, number, boolean] {
  t >>>= 0
  switch (kind) {
    case 1: return [0, WAVE[(sx * 2 + t * 2) & 63], true]
    case 2: { const p = (t * 2 + 512 - ci * 6) & 63; return [0, p < 32 ? -HOP[p] : 0, true] }
    case 3: {
      const h = Math.imul((ci * 31 + Math.floor(t / 3) * 977 + 1) >>> 0, 0x9e3779b1) >>> 16
      return [(h % 3) - 1, (Math.floor(h / 3) % 3) - 1, true]
    }
    case 4: return [0, 0, (Math.floor(t / 12) & 1) === 0]
    default: return [0, 0, true]
  }
}

/** motion_blit: the source rect with its moving characters displaced for step t, blitted in runs of columns of one
 *  effect word by blit(copy, rect, x, word) (x: the run's offset in the rect; word: the line's, or the letters' fill
 *  while on); false when no span of src is in the rect */
export function motionBlit(src: Img, r: number[], t: number, effect: number,
                           blit: (copy: Img, rect: number[], x: number, word: number, tex?: [number, number]) => void): boolean {
  const spans = motions.get(src.d)
  const [rx, ry, rw, rh] = r
  if (!spans || !spans.length || rw <= 0 || rh <= 0 || !spans.some((s) => s.x1 > rx && s.x0 < rx + rw)) return false
  const live = animLive(src)
  for (const rec of live) animPut(src, rec, t)            // animated emoji: their current frame goes into the copy
  const copy: Img = { w: rw, h: rh, d: new Uint16Array(rw * rh) }
  const words = new Array<number>(rw)
  const shifts = new Array<[number, number] | null>(rw)   // a letter's column: [dx, dy], so that its fill goes with it
  for (let x = 0; x < rw; x++) {
    const sx = rx + x, span = spans.find((s) => sx >= s.x0 && sx < s.x1)
    const [dx, dy, vis] = span ? fxOffset(span, sx - spans[0].x0, span.ci, t) : [0, 0, true]
    words[x] = span && span.hasFill && spanOn(span, t) ? (((span.fill << 16) | (effect & 0xffff)) >>> 0) : effect >>> 0
    shifts[x] = span ? [dx, dy] : null
    for (let y = 0; y < rh; y++) {
      const fx = sx - dx, fy = ry + y - dy
      if (vis && fx >= 0 && fx < src.w && fy >= 0 && fy < src.h && (!span || (fx >= span.x0 && fx < span.x1)))
        copy.d[y * rw + x] = src.d[fy * src.w + fx]
    }
  }
  for (let x = 0; x < rw; ) {                             // columns of one effect word: one blit
    let x1 = x + 1
    const s0 = shifts[x]
    while (x1 < rw && words[x1] === words[x] && shifts[x1]?.[0] === s0?.[0] && shifts[x1]?.[1] === s0?.[1]) x1++
    blit(copy, [x, 0, x1 - x, rh], x, words[x], s0 ? [rx + x - s0[0] - spans[0].x0, -s0[1]] : undefined)
    x = x1
  }
  for (const rec of live) animRestore(src, rec)          // frame 0 back, as drawn
  return true
}


// ---- animated emoji (native_emoji.c: emoji_draw's records, ext_glyph_blit_effect's overlay) ----
const ANIM_RECS = 32
interface AnimRec { x: number; y: number; cell: number; sheet: EmojiSheet }
/** per text image (the firmware's ring is global; one per image here, so that the studio's previews leave each
 *  other alone: the same while fewer than ANIM_RECS animated cells are on screen) */
const recs = new WeakMap<Uint16Array, { r: (AnimRec | null)[]; next: number }>()

function noteAnim(im: Img, sheet: EmojiSheet, x: number, y: number, cell: number) {
  let ring = recs.get(im.d)
  if (!ring) recs.set(im.d, (ring = { r: Array(ANIM_RECS).fill(null), next: 0 }))
  const hit = ring.r.find((r) => r && r.x === x && r.y === y)
  if (hit) { hit.cell = cell; hit.sheet = sheet; return }
  ring.r[ring.next++ % ANIM_RECS] = { x, y, cell, sheet }
}

/** each (cell pixel offset, x, y) of a record's frame-0 opaque pixels inside src */
function* recPixels(src: Img, r: AnimRec) {
  const s = r.sheet.size, f0 = r.sheet.frames[r.cell][0]
  for (let dy = 0; dy < s; dy++)
    for (let dx = 0; dx < s; dx++) {
      const px = r.x + dx, py = r.y + dy
      if (f0[dy * s + dx] >= 0 && px >= 0 && px < src.w && py >= 0 && py < src.h) yield [dy * s + dx, px, py] as const
    }
}

function animLive(src: Img): AnimRec[] {
  const ring = src.w === TEXT_IMG_W ? recs.get(src.d) : undefined
  const live: AnimRec[] = []
  if (ring)
    for (const rec of ring.r) {
      if (!rec || rec.cell >= rec.sheet.frames.length) continue
      const f0 = rec.sheet.frames[rec.cell][0]
      let ok = true
      for (const [k, px, py] of recPixels(src, rec)) if (src.d[py * src.w + px] !== ((f0[k] | EMOJI_TAG) >>> 0)) { ok = false; break }
      if (ok) live.push(rec)
    }
  return live
}
/** rec_put(0) then rec_put_now: the record's current frame, tagged, over its frame 0 */
function animPut(src: Img, rec: AnimRec, t: number) {
  for (const [, px, py] of recPixels(src, rec)) src.d[py * src.w + px] = 0
  const fr = rec.sheet.frames[rec.cell], ms = rec.sheet.ms[rec.cell], s = rec.sheet.size
  const f = fr[ms ? Math.floor(((t >>> 0) * 25 >>> 0) / ms) % fr.length : 0]
  for (let dy = 0; dy < s; dy++)
    for (let dx = 0; dx < s; dx++) {
      const px = rec.x + dx, py = rec.y + dy, v = f[dy * s + dx]
      if (v >= 0 && px >= 0 && px < src.w && py >= 0 && py < src.h) src.d[py * src.w + px] = v | EMOJI_TAG
    }
}
function animRestore(src: Img, rec: AnimRec) {
  const s = rec.sheet.size
  for (let dy = 0; dy < s; dy++)                           // rec_clear_box: the emoji pixels off, then frame 0 back
    for (let dx = 0; dx < s; dx++) {
      const px = rec.x + dx, py = rec.y + dy
      if (px >= 0 && px < src.w && py >= 0 && py < src.h && (src.d[py * src.w + px] & EMOJI_TAG)) src.d[py * src.w + px] = 0
    }
  const f0 = rec.sheet.frames[rec.cell][0]
  for (const [k, px, py] of recPixels(src, rec)) src.d[py * src.w + px] = f0[k] | EMOJI_TAG
}

/** glyph_blit_effect around animated cells: the live records of src (still holding frame 0) are taken out of the
 *  mask for blit(), put back, and their frame (t * 25 / ms) % frames drawn over dst in rect r placed at pos. */
export function animatedBlit(src: Img, dst: Img, r: number[], pos: [number, number], t: number, blit: () => void) {
  const ring = src.w === TEXT_IMG_W ? recs.get(src.d) : undefined
  const live: AnimRec[] = []
  if (ring)
    for (const rec of ring.r) {
      if (!rec || rec.cell >= rec.sheet.frames.length) continue
      const f0 = rec.sheet.frames[rec.cell][0]
      let ok = true
      for (const [k, px, py] of recPixels(src, rec)) if (src.d[py * src.w + px] !== ((f0[k] | EMOJI_TAG) >>> 0)) { ok = false; break }
      if (!ok) continue
      live.push(rec)
      for (const [, px, py] of recPixels(src, rec)) src.d[py * src.w + px] = 0
    }
  blit()
  for (const rec of live) {
    const f0 = rec.sheet.frames[rec.cell][0], s = rec.sheet.size
    for (const [k, px, py] of recPixels(src, rec)) src.d[py * src.w + px] = f0[k] | EMOJI_TAG
    const fr = rec.sheet.frames[rec.cell], ms = rec.sheet.ms[rec.cell]
    const f = fr[ms ? Math.floor(((t >>> 0) * 25 >>> 0) / ms) % fr.length : 0]
    for (let dy = 0; dy < s; dy++)
      for (let dx = 0; dx < s; dx++) {
        const sx = rec.x + dx, sy = rec.y + dy, x = pos[0] + sx - r[0], y = pos[1] + sy - r[1], v = f[dy * s + dx]
        if (v >= 0 && sx >= r[0] && sx < r[0] + r[2] && sy >= r[1] && sy < r[1] + r[3] && x >= 0 && x < dst.w && y >= 0 && y < dst.h)
          dst.d[y * dst.w + x] = v
      }
  }
}
