// Text effects: effect words FG<<16 | BG and the outline (docs/EFFECTS-AND-LAYERS.md), ported from sign_emu.py
// (effect_resolve 0x40D10, glyph_desc_from_effect 0x3EA28, glyph_blit 0x3EBF0, glyph_blit_effect 0x40820,
// glyph_outline_blit 0x3FF0C, flc_tick 0x42314), which is checked pixel for pixel against the firmware.

import { Flic } from "./flic.ts"
import type { Img } from "./image.ts"
import { animatedBlit, motionBlit } from "./native.ts"

export const FLC_SPEED_THRESHOLDS = [...Array.from({ length: 40 }, (_, i) => 38 + 25 * i), 0]
export const CYCLE_COLOURS = [0x7c00, 0x03e0, 0x7fe0]          // red, green, yellow
export const OUTLINE_STYLES = ["None", "Thick", "Thin", "Drop Shadow"]
export const OUTLINE_COLOURS = ["Black", "Red", "Green", "Yellow", "Blue", "Cyan", "Magenta", "White"]
export const TRANSPARENT = 0xffff                              // what effect_resolve makes of 0x8000

export function flcSpeedToHold(speed: number): number {
  if (speed <= FLC_SPEED_THRESHOLDS[0]) return 0
  let i = 1
  while (FLC_SPEED_THRESHOLDS[i] < speed && i < 40) i++
  return i
}

export function colourIndexToRgb555(i: number): number {
  return i >= 0 && i < 8 ? [0x0000, 0x7c00, 0x03e0, 0x7fe0, 0x001f, 0x03ff, 0x7c1f, 0x7fff][i] : 0
}

/** One effect animation slot (code 0xC000|n): effectNN.flc as effect_resolve drives it. */
export class EffectSlot {
  stamp = 0
  failed: boolean
  image: Img
  fl: Flic | null = null
  offsets: number[] = []
  n = 0
  hold = 0
  index = 0
  count = 0
  last = 0

  constructor(data?: Uint8Array | null) {
    this.failed = !data
    if (!data) {
      this.image = { w: 1, h: 1, d: Uint16Array.of(0x7fe0) }
      return
    }
    this.fl = new Flic(data)
    this.offsets = this.fl.frameOffsets()
    this.n = this.fl.frames
    this.hold = flcSpeedToHold(this.fl.speedMs)
    this.image = { w: this.fl.w, h: this.fl.h, d: new Uint16Array(this.fl.w * this.fl.h) }
  }

  rewind() {
    if (!this.failed) this.index = 0
  }

  /** flc_tick: 1 playing, 2 decoded the last frame, 0 failed. */
  tick(fc: number): number {
    if (this.failed) return 0
    if (((this.last + 8) >>> 0) < fc) this.rewind()
    this.last = fc
    if (this.count) {
      this.count--
      return 1
    }
    this.count = this.hold
    const fl = this.fl!
    if (this.index >= this.offsets.length) {
      this.failed = true
      this.image = { w: this.image.w, h: this.image.h, d: new Uint16Array(this.image.w * this.image.h).fill(fl.depth !== 16 ? 0x7fe0 : 0) }
      return 0
    }
    fl.decode(this.offsets[this.index])
    this.image = { w: fl.w, h: fl.h, d: fl.image555() }
    if (this.index < this.n - 1) {
      this.index++
      return 1
    }
    return 2
  }
}

/** A resolved half of an effect word: a solid colour (tex null; TRANSPARENT leaves the pixel) or a texture. */
export interface Layer {
  colour: number
  tex: Img | null
  anchored: number
}
const layer = (colour = 0, tex: Img | null = null, anchored = 1): Layer => ({ colour, tex, anchored })

/** rect_clip (0x3E6E0): clip r to bounds in place; edges that only touch still count. */
export function rectClip(b: [number, number, number, number], r: number[]): boolean {
  const [bx, by, bw, bh] = b
  if (!(r[1] <= by + bh && by <= r[1] + r[3] && r[0] <= bx + bw && bx <= r[0] + r[2])) return false
  if (r[0] < bx) { r[2] = r[0] + r[2] - bx; r[0] = bx }
  if (bx + bw < r[0] + r[2]) r[2] = bx + bw - r[0]
  if (r[1] < by) { r[3] = r[1] + r[3] - by; r[1] = by }
  if (by + bh < r[1] + r[3]) r[3] = by + bh - r[1]
  return true
}

/** The texture pixel at (x, y) of the block that starts at origin (coordinates taken as unsigned 32-bit). */
function tileAt(tex: Img, ox: number, oy: number, x: number, y: number): number {
  const row = (((oy >>> 0) % tex.h) + y) % tex.h, col = (((ox >>> 0) % tex.w) + x) % tex.w
  return tex.d[row * tex.w + col]
}

export class Effects {
  frameCount: number
  /** the clock of animated emoji: the step count of the line (or app) being drawn (apps.c emoji_anim_time) */
  emojiT = 0
  slots = new Map<number, EffectSlot>()

  /** slots: effectNN.flc bytes by n (the ones that loaded); every other slot draws solid yellow. */
  constructor(slots?: Map<number, Uint8Array> | null, frameCount = 0) {
    this.frameCount = frameCount
    if (slots) for (let n = 0; n < 100; n++) this.slots.set(n, new EffectSlot(slots.get(n)))
  }

  slot(n: number): EffectSlot {
    let s = this.slots.get(n)
    if (!s) this.slots.set(n, (s = new EffectSlot(null)))
    return s
  }

  /** effect_texture_8001: a 1-px column, h high, wiping down one row per 4 frames through red -> yellow -> green. */
  texture8001(h: number, shift = 2): Img {
    const t = Math.floor(this.frameCount / 2 ** shift)
    const [a, b] = ([[0x7c00, 0x03e0], [0x7fe0, 0x7c00], [0x03e0, 0x7fe0]] as const)[Math.floor(t / h) % 3]
    const d = new Uint16Array(h)
    for (let i = 0; i < h; i++) d[i] = i > t % h ? b : a
    return { w: 1, h, d }
  }

  /** effect_texture_8002: diagonal red/green/yellow stripes, each n wide, a 3n x 3n tile moving 1 px per 2 frames. */
  texture8002(n = 0x304, shift = 1): Img {
    n = Math.min(Math.max(n & 0xff, 1), 8)
    const w = 3 * n, t = Math.floor(this.frameCount / 2 ** shift)
    const d = new Uint16Array(w * w)
    for (let y = 0; y < w; y++)
      for (let x = 0; x < w; x++) d[y * w + x] = CYCLE_COLOURS[Math.floor((((x + y - t) >>> 0) % w) / n)]
    return { w, h: w, d }
  }

  /** effect_resolve: [ok, layer] */
  resolve(code: number, phase = 0, cellH = 16): [boolean, Layer] {
    const fc = this.frameCount
    if (code < 0x8000) return [true, layer(code)]
    if ((code & 0xc000) === 0x8000) {
      if (code === 0x8002) return [true, layer(0, this.texture8002())]
      if (code === 0xbffe || code === 0xbfff) {
        const v = ((fc >> 4) + (code === 0xbffe ? phase : -phase)) >>> 0
        return [true, layer(CYCLE_COLOURS[v % 3])]
      }
      if (code === 0x8000) return [true, layer(TRANSPARENT)]
      if (code === 0x8001) return [true, layer(0, this.texture8001(cellH), 0)]
      return [false, layer(0x7fe0)]                            // 0x8003..0xBFFD: nothing is drawn
    }
    const n = code & 0x3fff
    if (n >= 100) return [true, layer(0x7fe0)]
    const s = this.slot(n)
    if (s.stamp !== fc) {
      s.stamp = fc
      if (s.tick(fc) !== 1) s.rewind()
    }
    return [true, layer(0, s.image, 0)]   // anchored 0: the fill moves WITH the text, not fixed to the panel
  }

  /** glyph_desc_from_effect: [fg, bg], or null (nothing is drawn); the BG half is not resolved when FG fails. */
  desc(word: number, phase = 0, cellH = 16): [Layer, Layer] | null {
    const [ok, fg] = this.resolve(word >>> 16, phase, cellH)
    if (!ok) return null
    const [ok2, bg] = this.resolve(word & 0xffff, phase, cellH)
    return ok2 ? [fg, bg] : null
  }

  /** glyph_blit mode 8: src as a mask (pixel != 0); r the source rect (clipped in place), pos the destination. */
  blit(src: Img, dst: Img, desc: [Layer, Layer], r: number[], pos: [number, number],
       fgOrigin: [number, number], bgOrigin: [number, number]) {
    if (!rectClip([0, 0, src.w, src.h], r)) return
    const d = [pos[0], pos[1], r[2], r[3]]
    if (!rectClip([0, 0, dst.w, dst.h], d)) return
    r[0] -= pos[0] - d[0]
    r[1] -= pos[1] - d[1]
    let fo: [number, number] = [d[0] - pos[0] + fgOrigin[0], d[1] - pos[1] + fgOrigin[1]]
    let bo: [number, number] = [d[0] - pos[0] + bgOrigin[0], d[1] - pos[1] + bgOrigin[1]]
    const [fg, bg] = desc
    const [x, y, w, h] = d
    if (w <= 0 || h <= 0) return
    if (!fg.anchored) fo = [0, 0]
    if (!bg.anchored) bo = [0, 0]
    // the numpy slices: the mask block is cut short at src's edge, the output block at dst's
    const mh = Math.min(h, Math.max(0, src.h - r[1])), mw = Math.min(w, Math.max(0, src.w - r[0]))
    const mask = (u: number, v: number) => (v < mh && u < mw ? src.d[(r[1] + v) * src.w + r[0] + u] !== 0 : false)
    for (let v = 0; v < h; v++)
      for (let u = 0; u < w; u++) {
        const i = (y + v) * dst.w + x + u, m = mask(u, v)
        if (fg.tex) {
          const t = tileAt(fg.tex, fo[0], fo[1], u, v)
          if (!bg.tex && bg.colour === TRANSPARENT) {
            if (m) dst.d[i] = t
          } else dst.d[i] = m ? t : 0
        } else {
          const behind = !bg.tex ? bg.colour : 0
          if (behind === TRANSPARENT) {
            if (m) dst.d[i] = fg.colour
          } else dst.d[i] = m ? fg.colour : behind
        }
        if (!m) {
          if (bg.tex) dst.d[i] = tileAt(bg.tex, bo[0], bo[1], u, v)
          else if (fg.tex && bg.colour !== TRANSPARENT) dst.d[i] = bg.colour
        }
      }
  }

  /** glyph_blit_effect: phase 0, the rect's height as the cell height, pos as both texture origins. */
  blitEffect(src: Img, dst: Img, word: number, r: number[], pos: [number, number]) {
    // moving characters first (native_emoji.c motion_blit), else animated emoji around the plain blit
    if (motionBlit(src, [...r], this.emojiT, word >>> 0,
                   (copy, rect, x, w, tex) => this.blitPlain(copy, dst, w, rect, [pos[0] + x, pos[1]], tex))) return
    animatedBlit(src, dst, [...r], pos, this.emojiT, () => this.blitPlain(src, dst, word, r, pos))
  }

  /** a mask blit with the fill's texture taken from (col, row) at the rect's first pixel: a moving letter carries its
   *  fill (glyph_blit_effect_tex) */
  blitTex(src: Img, dst: Img, word: number, r: number[], pos: [number, number], tex: [number, number]) {
    this.blitPlain(src, dst, word, r, pos, tex)
  }

  private blitPlain(src: Img, dst: Img, word: number, r: number[], pos: [number, number], tex?: [number, number]) {
    r = [...r]
    const h = r[3] & 0xffff
    const dsc = this.desc(word >>> 0, 0, h & 0x8000 ? h - 0x10000 : h)
    if (dsc && tex) {
      const [fg, bg] = dsc, t = fg.tex
      const o: [number, number] = t ? [((tex[0] % t.w) + t.w) % t.w, ((tex[1] % t.h) + t.h) % t.h] : pos
      this.blit(src, dst, [{ ...fg, anchored: 1 }, bg], r, pos, o, pos)
    } else if (dsc) this.blit(src, dst, dsc, r, pos, pos, pos)
    // ext_glyph_blit_effect (native emoji): the tagged emoji pixels of the source get their own colour back
    for (let y = 0; y < r[3]; y++) {
      const sy = r[1] + y, dy = pos[1] + y
      if (sy < 0 || sy >= src.h || dy < 0 || dy >= dst.h) continue
      for (let x = 0; x < r[2]; x++) {
        const sx = r[0] + x, dx = pos[0] + x
        if (sx < 0 || sx >= src.w || dx < 0 || dx >= dst.w) continue
        const p = src.d[sy * src.w + sx]
        if (p & 0x8000) dst.d[dy * dst.w + dx] = p & 0x7fff
      }
    }
  }
}

/** glyph_outline_blit: an outline around the mask pixels of rect sr of src, drawn into dst at pos, in
 *  colourIndexToRgb555(colour); rows outside clip are not written, columns limited at clip's edges. Styles: 1 Thick,
 *  2 Thin, 3 Drop Shadow. Run by run, as the firmware does. */
export function outlineBlit(src: Img, sr: number[], dst: Img, pos: [number, number], style: number, colour: number,
                            clip: [number, number, number, number]) {
  const c = colourIndexToRgb555(colour)
  const dw = dst.w, sw = src.w
  const [px, py] = pos
  const xr = clip[0] + clip[2] - 1, yb = clip[1] + clip[3] - 1
  const put = (x: number, y: number) => {
    const i = y * dw + x
    if (i >= 0 && i < dst.d.length) dst.d[i] = c
  }
  const ink = (u: number, v: number) => {
    const i = (sr[1] + v) * sw + sr[0] + u
    return i >= 0 && i < src.d.length && src.d[i] !== 0
  }
  const inr = (y: number) => clip[1] <= y && y <= yb
  for (let v = 0; v < Math.max(sr[3], 0); v++) {
    const Y = v + py
    const up = inr(Y - 1), mid = inr(Y), dn = inr(Y + 1)
    const last = sr[2] - 1
    if (style === 1 && 0 <= Y && Y <= yb) {
      let inside = false
      for (let u = 0; u < sr[2]; u++) {
        const X = u + px, notright = X < xr, atleft = X <= clip[0]
        if (inside) {
          if (!ink(u, v)) {
            inside = false
            if (notright) {
              if (up) put(X, Y - 1)
              if (mid) put(X, Y)
              if (dn) put(X, Y + 1)
            }
          } else {
            if (up) put(X, Y - 1)
            if (dn) put(X, Y + 1)
          }
        } else if (ink(u, v)) {
          inside = true
          if (up) {
            put(X, Y - 1)
            if (!atleft) put(X - 1, Y - 1)
          }
          if (mid && !atleft) put(X - 1, Y)
          if (dn) {
            if (!atleft) put(X - 1, Y + 1)
            put(X, Y + 1)
          }
        }
        if (u === last && inside && notright) {
          if (up) put(X + 1, Y - 1)
          if (mid) put(X + 1, Y)
          if (dn) put(X + 1, Y + 1)
        }
      }
    } else if (style === 2 && 0 <= Y && Y <= yb) {
      let inside = false
      for (let u = 0; u < sr[2]; u++) {
        const X = u + px, atright = xr <= X
        if (inside) {
          if (!ink(u, v)) {
            inside = false
            if (mid && !atright) put(X, Y)
          } else {
            if (up) put(X, Y - 1)
            if (dn) put(X, Y + 1)
          }
        } else if (ink(u, v)) {
          inside = true
          if (up) put(X, Y - 1)
          if (mid && !(X <= clip[0])) put(X - 1, Y)
          if (dn) put(X, Y + 1)
        }
        if (u === last && inside && mid && !atright) put(X + 1, Y)
      }
    } else if (style === 3 && Y <= yb) {
      let state = 0
      for (let u = 0; u < sr[2]; u++) {
        const X = u + px, atright = xr <= X
        if (state === 0) state = ink(u, v) ? 1 : 0
        else {
          state = ink(u, v) ? 1 : 0
          if (dn && !atright) put(X, Y + 1)
        }
        if (u === last && state && dn && !atright) put(X + 1, Y + 1)
      }
    }
  }
}
