// Animated GIF from RGB555 frames, in the browser: Compose's "Save GIF" (the frames it previews, the LED pixels scaled
// up square). GIF89a, looping, one colour table per frame (a panel frame rarely has more than a few dozen colours;
// past 256 the colours are cut to 3-3-2 bits), LZW as the format defines it.
import type { Img } from "./image.ts"

const rgb = (v: number): [number, number, number] => [((v >> 10) & 31) * 255 / 31 | 0, ((v >> 5) & 31) * 255 / 31 | 0, (v & 31) * 255 / 31 | 0]

function lzw(indices: Uint8Array, minCode: number): number[] {
  const out: number[] = []
  let cur = 0, bits = 0
  const clear = 1 << minCode, end = clear + 1
  let size = minCode + 1, next = end + 1
  let dict = new Map<number, number>()
  const emit = (code: number) => {
    cur |= code << bits
    bits += size
    while (bits >= 8) {
      out.push(cur & 0xff)
      cur >>>= 8
      bits -= 8
    }
  }
  emit(clear)
  let prefix = indices[0]
  for (let i = 1; i < indices.length; i++) {
    const k = indices[i], key = (prefix << 8) | k
    const hit = dict.get(key)
    if (hit !== undefined) {
      prefix = hit
      continue
    }
    emit(prefix)
    if (next < 4096) {
      dict.set(key, next++)
      if (next > 1 << size && size < 12) size++
    } else {
      emit(clear)
      dict = new Map()
      size = minCode + 1
      next = end + 1
    }
    prefix = k
  }
  emit(prefix)
  emit(end)
  if (bits > 0) out.push(cur & 0xff)
  return out
}

/** frames: RGB555 images; scale: each LED pixel as scale x scale; delayCs: per frame, in hundredths of a second. */
export function encodeGif(frames: Img[], scale = 8, delayCs = 5): Uint8Array {
  const W = frames[0].w * scale, H = frames[0].h * scale
  const out: number[] = []
  const u16 = (v: number) => out.push(v & 0xff, (v >> 8) & 0xff)
  out.push(...Array.from("GIF89a", (c) => c.charCodeAt(0)))
  u16(W); u16(H)
  out.push(0x00, 0, 0)                                             // no global colour table
  out.push(0x21, 0xff, 11, ...Array.from("NETSCAPE2.0", (c) => c.charCodeAt(0)), 3, 1, 0, 0, 0)   // loop forever
  for (const f of frames) {
    let cols = [...new Set(f.d)]
    let map = (v: number) => v
    if (cols.length > 256) {                                       // 3-3-2 bits
      map = (v: number) => (v & 0x7000) | (v & 0x0380) | (v & 0x0018)
      cols = [...new Set(Array.from(f.d, map))]
    }
    const index = new Map(cols.map((c, i) => [c, i]))
    let bitsPer = 1
    while (1 << bitsPer < cols.length) bitsPer++
    const n = 1 << bitsPer
    out.push(0x21, 0xf9, 4, 0x00); u16(delayCs); out.push(0, 0)  // graphic control: delay
    out.push(0x2c); u16(0); u16(0); u16(W); u16(H); out.push(0x80 | (bitsPer - 1))   // image, local table
    for (let i = 0; i < n; i++) out.push(...(i < cols.length ? rgb(cols[i]) : [0, 0, 0]))
    const px = new Uint8Array(W * H)
    for (let y = 0; y < H; y++) {
      const row = Math.floor(y / scale) * f.w
      for (let x = 0; x < W; x++) px[y * W + x] = index.get(map(f.d[row + Math.floor(x / scale)]))!
    }
    const minCode = Math.max(2, bitsPer)
    out.push(minCode)
    const data = lzw(px, minCode)
    for (let i = 0; i < data.length; i += 255) {
      const chunk = data.slice(i, i + 255)
      out.push(chunk.length, ...chunk)
    }
    out.push(0)
  }
  out.push(0x3b)
  return Uint8Array.from(out)
}
