// MQF1 fonts (studio/marquee_studio/mqf.py) as our firmware loads them (firmware/ext/fonts.c): each glyph's advance
// box in a 16 x 16 cell grid (cell: the widest advance x the line height), the stock measuring, then the font's own
// metrics: every code's width = its advance (0 if the font lacks it), left 0, not proportional. Flag 2 (proportional
// grid) keeps the stock measuring (left bearing, 1 px gaps): the sign's own proportional fonts, fonte01..04.
import { FwFont } from "./font.ts"

async function inflate(z: Uint8Array): Promise<Uint8Array> {
  const ds = new DecompressionStream("deflate")
  return new Uint8Array(await new Response(new Blob([z as BlobPart]).stream().pipeThrough(ds)).arrayBuffer())
}

export async function mqfFont(data: Uint8Array, name = ""): Promise<FwFont> {
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength)
  if (dv.getUint32(0) !== 0x4d514631) throw new Error(`${name}: not an MQF1 font`)
  const lh = data[4], flags = data[6], nr = dv.getUint16(8), ng = dv.getUint16(10), blen = dv.getUint32(12)
  const body = flags & 1 ? await inflate(data.subarray(16)) : data.subarray(16, 16 + blen)
  const bv = new DataView(body.buffer, body.byteOffset, body.byteLength)
  const adv = body.subarray(8 * nr, 8 * nr + ng)
  let cw = 1
  for (const a of adv) cw = Math.max(cw, a)
  const W = 16 * cw, H = 16 * lh
  const rgba = new Uint8Array(W * H * 4)
  const advOf = new Array<number>(256).fill(0)
  const cellAt: number[] = []                                  // each glyph's cell bytes offset
  let off = 8 * nr + ng
  for (let g = 0; g < ng; g++) {
    cellAt.push(off)
    off += lh * ((adv[g] + 7) >> 3)
  }
  for (let r = 0; r < nr; r++) {
    const first = bv.getUint32(8 * r), count = bv.getUint16(8 * r + 4), g0 = bv.getUint16(8 * r + 6)
    for (let k = 0; k < count; k++) {
      const c = first + k, g = g0 + k, a = adv[g], nb = (a + 7) >> 3
      if (c >= 256) continue
      const x0 = (c & 15) * cw, y0 = (c >> 4) * lh
      for (let y = 0; y < lh; y++)
        for (let x = 0; x < a; x++)
          if (body[cellAt[g] + y * nb + (x >> 3)] & (0x80 >> (x & 7))) {
            const p = ((y0 + y) * W + x0 + x) * 4
            rgba[p] = rgba[p + 1] = rgba[p + 2] = 255
            rgba[p + 3] = 255
          }
      advOf[c] = a
    }
  }
  const f = new FwFont({ w: W, h: H, data: rgba }, name)
  if (!(flags & 2))                                              // flag 2 (proportional grid): keep the stock measuring
    for (let c = 0; c < 256; c++) {
      f.left[c] = 0
      f.width[c] = advOf[c]
      f.right[c] = (cw - advOf[c]) & 0xff
      f.prop[c] = 0
    }
  return f
}
