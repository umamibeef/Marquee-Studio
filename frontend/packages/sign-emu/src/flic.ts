// Autodesk FLIC (.flc/.fli) as the sign plays it (sign_emu.FLIC): COLOR256/64, BRUN, LC, SS2, BLACK, COPY, and the
// sign's 16-bit variant (BRUN over 2*width bytes per row, COPY of little-endian RGB555).

const u16 = (d: Uint8Array, o: number) => d[o] | (d[o + 1] << 8)
const u32 = (d: Uint8Array, o: number) => (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0
const i8 = (v: number) => (v & 0x80 ? v - 0x100 : v)

export class Flic {
  d: Uint8Array
  frames: number
  w: number
  h: number
  depth: number
  speedMs: number
  palette: [number, number, number][] = Array.from({ length: 256 }, () => [0, 0, 0])
  pixels: number[]

  constructor(data: Uint8Array) {
    this.d = data
    const magic = u16(data, 4)
    if (magic !== 0xaf11 && magic !== 0xaf12) throw new Error(`not FLIC: ${magic.toString(16)}`)
    this.frames = u16(data, 6); this.w = u16(data, 8); this.h = u16(data, 10); this.depth = u16(data, 12)
    const speed = u32(data, 0x10)
    this.speedMs = magic === 0xaf11 ? Math.floor((speed * 100) / 7) : speed
    this.pixels = new Array(this.w * this.h).fill(0)
  }

  /** File offsets of the frame chunks (0xF1FA), in order, from oframe1 (header +0x50). */
  frameOffsets(): number[] {
    const d = this.d
    const oframe1 = d.length >= 0x54 ? u32(d, 0x50) : 0
    let off = oframe1 >= 128 && oframe1 < d.length ? oframe1 : 128
    const out: number[] = []
    while (off + 16 <= d.length) {
      const fsize = u32(d, off), ftype = u16(d, off + 4)
      if (ftype === 0xf1fa) out.push(off)
      off += fsize ? fsize : 16
    }
    return out
  }

  /** Apply the frame chunk at off to the current pixels and palette (frames are deltas). */
  decode(off: number) {
    const d = this.d
    const fsize = u32(d, off), nsub = u16(d, off + 6)
    let p = off + 16
    for (let k = 0; k < nsub; k++) {
      if (p + 6 > off + fsize) break
      const csz = u32(d, p), ctyp = u16(d, p + 4), body = p + 6
      if (this.depth === 16) {
        if (ctyp === 15) this.brun16(body)
        else if (ctyp === 16) {
          const px: number[] = []
          for (let i = 0; i < this.w * this.h; i++) px.push(u16(d, body + 2 * i))
          this.pixels = px
        } else if (ctyp === 13) this.pixels = new Array(this.w * this.h).fill(0)
        p += csz ? csz : 6
        continue
      }
      if (ctyp === 4 || ctyp === 11) this.palette8(body, ctyp === 11)
      else if (ctyp === 15) this.brun(body)
      else if (ctyp === 12) this.lc(body)
      else if (ctyp === 7) this.ss2(body)
      else if (ctyp === 13) this.pixels = new Array(this.w * this.h).fill(0)
      else if (ctyp === 16) this.pixels = Array.from(d.subarray(body, body + this.w * this.h))
      p += csz ? csz : 6
    }
  }

  private palette8(off: number, div64: boolean) {
    const d = this.d
    const npk = u16(d, off)
    off += 2
    let idx = 0
    for (let k = 0; k < npk; k++) {
      const skip = d[off], cnt = d[off + 1]
      off += 2
      idx += skip
      const n = cnt ? cnt : 256
      for (let j = 0; j < n; j++) {
        let r = d[off], g = d[off + 1], b = d[off + 2]
        off += 3
        if (div64) { r <<= 2; g <<= 2; b <<= 2 }
        if (idx < 256) this.palette[idx] = [r, g, b]
        idx++
      }
    }
  }

  private brun(off: number) {
    const d = this.d, W = this.w
    for (let y = 0; y < this.h; y++) {
      off += 1
      const line: number[] = []
      while (line.length < W) {
        const c = d[off++]
        if (c & 0x80) {
          const n = 256 - c
          for (let i = 0; i < n; i++) line.push(d[off + i])
          off += n
        } else {
          for (let i = 0; i < c; i++) line.push(d[off])
          off += 1
        }
      }
      for (let x = 0; x < W; x++) this.pixels[y * W + x] = line[x]
    }
  }

  private brun16(off: number) {
    const d = this.d, W = this.w
    const raw: number[] = []
    for (let y = 0; y < this.h; y++) {
      off += 1
      const row: number[] = []
      while (row.length < 2 * W) {
        const c = d[off++]
        if (c & 0x80) {
          const n = 256 - c
          for (let i = 0; i < n; i++) row.push(d[off + i])
          off += n
        } else {
          for (let i = 0; i < c; i++) row.push(d[off])
          off += 1
        }
      }
      for (let i = 0; i < 2 * W; i++) raw.push(row[i])
    }
    this.pixels = []
    for (let i = 0; i < W * this.h; i++) this.pixels.push(raw[2 * i] | (raw[2 * i + 1] << 8))
  }

  private lc(off: number) {
    const d = this.d, px = this.pixels, W = this.w
    const y0 = u16(d, off), ny = u16(d, off + 2)
    off += 4
    let y = y0
    for (let k = 0; k < ny; k++) {
      const npk = d[off++]
      let x = 0
      for (let j = 0; j < npk; j++) {
        const skip = d[off], sz = i8(d[off + 1])
        off += 2
        x += skip
        if (sz >= 0) {
          for (let i = 0; i < sz; i++) px[y * W + x + i] = d[off + i]
          off += sz
          x += sz
        } else {
          const n = -sz, c = d[off++]
          for (let i = 0; i < n; i++) px[y * W + x + i] = c
          x += n
        }
      }
      y++
    }
  }

  private ss2(off: number) {
    const d = this.d, px = this.pixels, W = this.w
    const ny = u16(d, off)
    off += 2
    let y = 0, done = 0
    while (done < ny) {
      const word = u16(d, off)
      off += 2
      if (word & 0x8000) {
        if (word & 0x4000) {
          y += 0x10000 - word
          continue
        }
        px[y * W + (W - 1)] = word & 0xff
        done++
        y++
        continue
      }
      let x = 0
      for (let j = 0; j < word; j++) {
        const skip = d[off], sz = i8(d[off + 1])
        off += 2
        x += skip
        if (sz >= 0) {
          for (let i = 0; i < sz; i++) {
            px[y * W + x + i * 2] = d[off + i * 2]
            px[y * W + x + i * 2 + 1] = d[off + i * 2 + 1]
          }
          off += sz * 2
          x += sz * 2
        } else {
          const n = -sz, a = d[off], b = d[off + 1]
          off += 2
          for (let i = 0; i < n; i++) {
            px[y * W + x + i * 2] = a
            px[y * W + x + i * 2 + 1] = b
          }
          x += n * 2
        }
      }
      done++
      y++
    }
  }

  /** The current frame as the firmware holds it: RGB555 (8-bit through the palette, >> 3 per channel). */
  image555(): Uint16Array {
    const out = new Uint16Array(this.w * this.h)
    if (this.depth === 16) {
      for (let i = 0; i < out.length; i++) out[i] = this.pixels[i] & 0xffff
      return out
    }
    const pal = this.palette.map(([r, g, b]) => (((r >> 3) & 31) << 10) | (((g >> 3) & 31) << 5) | ((b >> 3) & 31))
    for (let i = 0; i < out.length; i++) out[i] = pal[this.pixels[i]]
    return out
  }
}
