// A small PNG decoder (non-interlaced, any colour type, 1..16-bit) for the sign's font sheets and animations. The
// inflate is the platform's DecompressionStream, so it runs in browsers and in Node alike.

export interface Rgba {
  w: number
  h: number
  /** w * h * 4 bytes, RGBA */
  data: Uint8Array
}

async function inflate(z: Uint8Array): Promise<Uint8Array> {
  const ds = new DecompressionStream("deflate")
  const out = new Blob([z as BlobPart]).stream().pipeThrough(ds)
  return new Uint8Array(await new Response(out).arrayBuffer())
}

function be32(b: Uint8Array, o: number) {
  return ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0
}

export async function decodePng(file: Uint8Array): Promise<Rgba> {
  const sig = [137, 80, 78, 71, 13, 10, 26, 10]
  if (!sig.every((v, i) => file[i] === v)) throw new Error("not a PNG")
  let w = 0, h = 0, depth = 0, ctype = 0, interlace = 0
  let palette = new Uint8Array(0), trns = new Uint8Array(0)
  const idat: Uint8Array[] = []
  for (let o = 8; o + 8 <= file.length; ) {
    const n = be32(file, o)
    const type = String.fromCharCode(file[o + 4], file[o + 5], file[o + 6], file[o + 7])
    const body = file.subarray(o + 8, o + 8 + n)
    if (type === "IHDR") {
      w = be32(body, 0); h = be32(body, 4); depth = body[8]; ctype = body[9]; interlace = body[12]
    } else if (type === "PLTE") palette = body.slice()
    else if (type === "tRNS") trns = body.slice()
    else if (type === "IDAT") idat.push(body)
    else if (type === "IEND") break
    o += 12 + n
  }
  if (interlace) throw new Error("interlaced PNG is not supported")
  const total = idat.reduce((s, b) => s + b.length, 0)
  const z = new Uint8Array(total)
  idat.reduce((at, b) => (z.set(b, at), at + b.length), 0)
  const raw = await inflate(z)
  const channels = ({ 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 } as Record<number, number>)[ctype]
  if (!channels) throw new Error(`PNG colour type ${ctype}`)
  const bpp = Math.max(1, (channels * depth) >> 3)          // bytes per pixel for the filters
  const stride = (w * channels * depth + 7) >> 3
  const rows = new Uint8Array(stride * h)
  let prev = new Uint8Array(stride)
  for (let y = 0, p = 0; y < h; y++) {
    const f = raw[p++]
    const cur = raw.subarray(p, p + stride)
    const out = rows.subarray(y * stride, (y + 1) * stride)
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? out[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0
      let v = cur[x]
      if (f === 1) v += a
      else if (f === 2) v += b
      else if (f === 3) v += (a + b) >> 1
      else if (f === 4) {
        const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c)
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c
      }
      out[x] = v & 0xff
    }
    prev = out
    p += stride
  }
  const data = new Uint8Array(w * h * 4)
  const sample = (row: Uint8Array, i: number) => {        // sample i of a row, scaled to 8 bits
    if (depth === 8) return row[i]
    if (depth === 16) return row[2 * i]
    const per = 8 / depth, byte = row[Math.floor(i / per)], shift = (per - 1 - (i % per)) * depth
    const v = (byte >> shift) & ((1 << depth) - 1)
    return ctype === 3 ? v : Math.round((v * 255) / ((1 << depth) - 1))
  }
  for (let y = 0; y < h; y++) {
    const row = rows.subarray(y * stride, (y + 1) * stride)
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 4
      if (ctype === 3) {
        const i = depth === 8 ? row[x] : sample(row, x)
        data[o] = palette[3 * i]; data[o + 1] = palette[3 * i + 1]; data[o + 2] = palette[3 * i + 2]
        data[o + 3] = i < trns.length ? trns[i] : 255
      } else if (ctype === 0 || ctype === 4) {
        const g = sample(row, x * channels)
        data[o] = data[o + 1] = data[o + 2] = g
        data[o + 3] = ctype === 4 ? sample(row, x * channels + 1) : 255
      } else {
        data[o] = sample(row, x * channels); data[o + 1] = sample(row, x * channels + 1)
        data[o + 2] = sample(row, x * channels + 2)
        data[o + 3] = ctype === 6 ? sample(row, x * channels + 3) : 255
      }
    }
  }
  return { w, h, data }
}
