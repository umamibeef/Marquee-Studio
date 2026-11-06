// RGB555 images (one Uint16 per pixel, row-major) and the C integer idioms the firmware port needs.

export interface Img {
  w: number
  h: number
  d: Uint16Array
}

export function img(w: number, h: number, fill = 0): Img {
  const d = new Uint16Array(w * h)
  if (fill) d.fill(fill)
  return { w, h, d }
}

export const U32 = 0xffffffff

/** (short) v */
export function s16(v: number): number {
  v = v & 0xffff
  return v & 0x8000 ? v - 0x10000 : v
}

/** (int) v */
export function s32(v: number): number {
  return v | 0
}

/** C integer division (truncates towards zero); division by 0 gives 0 (PowerPC divw is undefined there). */
export function cdiv(a: number, b: number): number {
  if (b === 0) return 0
  return Math.trunc(a / b)
}

/** (int)u >> 1, rounded towards zero */
export function half(u: number): number {
  return cdiv(s32(u), 2)
}

/** image_blit, RGB555 to RGB555: a plain copy, clipped. */
export function blit(src: Img, dst: Img, x: number, y: number) {
  const x1 = Math.max(0, x), y1 = Math.max(0, y)
  const x2 = Math.min(dst.w, x + src.w), y2 = Math.min(dst.h, y + src.h)
  for (let yy = y1; yy < y2; yy++) {
    const so = (yy - y) * src.w - x, dO = yy * dst.w
    for (let xx = x1; xx < x2; xx++) dst.d[dO + xx] = src.d[so + xx]
  }
}
