// A fonteNN.png as the firmware's font_build makes it (sign_emu.FwFont): layout 2, a 16 x 16 grid of all 256
// codes, cell = (w/16, h/16); a pixel is ink when its RGB555 value is not 0. Per code left/width/right/proportional
// (font_measure_glyph_maybe 0x427FC): the space is img_w/64 wide (at least 3) and fixed; '0'..'9', 0x10..0x19 and
// 0xFF the whole cell, fixed; anything else trimmed to its inked columns, proportional. Then 0..9 get the union of
// their extents.

import type { Img } from "./image.ts"
import type { Rgba } from "./png.ts"

export class FirmwareHang extends Error {}

export class FwFont {
  name: string
  w: number
  h: number
  cw: number
  ch: number
  /** w * h, 1 where there is ink */
  ink: Uint8Array
  left = new Array<number>(256).fill(0)
  width = new Array<number>(256).fill(0)
  right = new Array<number>(256).fill(0)
  prop = new Array<number>(256).fill(0)

  constructor(png: Rgba, name = "") {
    this.name = name
    this.w = png.w
    this.h = png.h
    this.ink = new Uint8Array(png.w * png.h)
    for (let i = 0; i < png.w * png.h; i++)
      this.ink[i] = png.data[4 * i] >> 3 || png.data[4 * i + 1] >> 3 || png.data[4 * i + 2] >> 3 ? 1 : 0
    this.cw = this.w >> 4
    this.ch = this.h >> 4
    for (let c = 0; c < 256; c++) this.measure(c)
    let lo = Infinity, hi = -Infinity
    for (let c = 0; c < 10; c++) {
      lo = Math.min(lo, this.left[c])
      hi = Math.max(hi, (this.left[c] + this.width[c]) & 0xff)
    }
    for (let c = 0; c < 10; c++) {
      this.left[c] = lo
      this.width[c] = (hi - lo) & 0xff
    }
  }

  private measure(c: number) {
    const cw = this.cw, ch = this.ch
    this.prop[c] = 1
    if (c === 0x20) {
      this.left[c] = 0
      this.width[c] = Math.max(3, (this.w >> 6) & 0xff)
      this.prop[c] = 0
      return
    }
    if (c === 0xff || (c >= 0x30 && c <= 0x39) || (c >= 0x10 && c <= 0x19)) {
      this.left[c] = 0; this.width[c] = cw & 0xff; this.right[c] = 0; this.prop[c] = 0
      return
    }
    const x0 = (c & 15) * cw, y0 = (c >> 4) * ch
    const cols = new Array<boolean>(cw).fill(false)
    for (let x = 0; x < cw; x++)
      for (let y = 0; y < ch; y++)
        if (this.ink[(y0 + y) * this.w + x0 + x]) {
          cols[x] = true
          break
        }
    let left = 0
    while (left < cw && !cols[left]) left++
    let right = 0
    if (left < cw) while (right < cw && !cols[cw - 1 - right]) right++
    if (left + right < cw) {
      this.left[c] = left; this.width[c] = cw - (left + right); this.right[c] = right
    } else {
      this.left[c] = cw & 0xff; this.width[c] = 0; this.right[c] = 0
    }
  }

  /** text_width (0x43BC0) */
  textWidth(data: ArrayLike<number>): number {
    let w = 0, prev = 1
    for (let i = 0; i < data.length; i++) {
      const c = data[i]
      if (this.prop[c]) w += (prev === 0 ? 1 : 0) + this.width[c] + 1
      else w += this.width[c]
      prev = this.prop[c]
    }
    return w
  }

  /** text_draw_proportional (0x42478): returns the drawn width (u16). */
  draw(im: Img, x: number, y: number, data: ArrayLike<number>, fg: number, bg: number): number {
    const x0 = x
    let prev = 1
    for (let i = 0; i < data.length; i++) {
      const c = data[i], p = this.prop[c]
      if (prev === 0 && p) {
        vline(im, x, y, this.ch, 0)
        x++
      }
      const gw = this.width[c]
      const sx = (c & 15) * this.cw + this.left[c], sy = (c >> 4) * this.ch
      this.blitMask(im, sx, sy, gw, x, y, fg, bg)
      x += gw
      if (p) {
        vline(im, x, y, this.ch, bg)
        x++
      }
      prev = p
    }
    return (x - x0) & 0xffff
  }

  /** one glyph's ink columns at x (font_draw_char_part with fg/bg), no separator columns */
  glyph(im: Img, x: number, y: number, c: number, fg: number, bg: number) {
    this.blitMask(im, (c & 15) * this.cw + this.left[c], (c >> 4) * this.ch, this.width[c], x, y, fg, bg)
  }

  /** glyph_blit mode 0 -> blit_mask_2colour of the ink rect (sx, sy, gw, ch), clipped (as a numpy slice). */
  private blitMask(im: Img, sx: number, sy: number, gw: number, x: number, y: number, fg: number, bg: number) {
    const mw = Math.max(0, Math.min(gw, this.w - sx)), mh = Math.max(0, Math.min(this.ch, this.h - sy))
    const x1 = Math.max(0, x), y1 = Math.max(0, y)
    const x2 = Math.min(im.w, x + mw), y2 = Math.min(im.h, y + mh)
    if (x2 <= x1 || y2 <= y1) return
    for (let yy = y1; yy < y2; yy++)
      for (let xx = x1; xx < x2; xx++)
        im.d[yy * im.w + xx] = this.ink[(sy + yy - y) * this.w + sx + xx - x] ? fg : bg
  }
}

/** draw_line from (x, y) to (x, y + n), each pixel clipped; an end outside the 16-bit range never comes. */
export function vline(im: Img, x: number, y: number, n: number, colour: number) {
  if (!(-0x8000 <= y + n && y + n <= 0x7fff)) throw new FirmwareHang(`draw_line to y = ${y + n} never ends`)
  if (x >= 0 && x < im.w)
    for (let yy = Math.max(0, y); yy < Math.min(im.h, y + n + 1); yy++) im.d[yy * im.w + x] = colour
}

/** The indices of fonts an automatic pick may choose (our firmware's fonts.auto, firmware/ext/fonts.c): those named
 *  in auto, in order; null (all of them: the stock rule) when auto is empty or names none of them. */
function autoIdx(fonts: FwFont[], auto?: ReadonlySet<string>): number[] | null {
  if (!auto || !auto.size) return null
  const idx = fonts.flatMap((f, i) => (auto.has(f.name) ? [i] : []))
  return idx.length ? idx : null
}

/** font_pick_largest (0x44750), mode 0: the widest font whose cell height fits; ties go to the later one.
 *  auto: the names it may pick (our firmware's fonts.auto). */
export function fontPickLargest(fonts: FwFont[], boxH: number, auto?: ReadonlySet<string>): number | null {
  const sub = autoIdx(fonts, auto)
  if (sub) {
    const k = fontPickLargest(sub.map((i) => fonts[i]), boxH)
    return k === null ? null : sub[k]
  }
  let bestW = 0, bestH = 0, fbW = 0, fbH = 0xffffffff
  let found: number | null = null, fb: number | null = null
  fonts.forEach((f, i) => {
    if (f.cw < bestW || boxH < f.ch || f.ch < bestH) {
      if (fbW < f.cw && f.ch <= fbH) {
        fb = i; fbH = f.ch; fbW = f.cw
      }
    } else {
      found = i; bestH = f.ch; bestW = f.cw
    }
  })
  return found !== null ? found : fb
}

/** text_draw_justified_maybe (0x43F20) for a centred segment: on its first character, the tallest font that fits. */
export function fontPickForText(fonts: FwFont[], text: ArrayLike<number>, boxW: number, boxH: number,
                                auto?: ReadonlySet<string>): number | null {
  const sub = autoIdx(fonts, auto)
  if (sub) {
    const k = fontPickForText(sub.map((i) => fonts[i]), text, boxW, boxH)
    return k === null ? null : sub[k]
  }
  let bestW = 0, bestH = 0, fbW = 0xffffffff, fbH = 0xffffffff
  let found: number | null = null, fb: number | null = null
  fonts.forEach((f, i) => {
    const w = f.textWidth(text), h = f.ch
    if (boxW < w || w < bestW || boxH < h || h < bestH) {
      if (w <= fbW && h <= fbH) {
        fb = i; fbH = h; fbW = w
      }
    } else {
      found = i; bestH = h; bestW = w
    }
  })
  return found !== null ? found : fb
}
