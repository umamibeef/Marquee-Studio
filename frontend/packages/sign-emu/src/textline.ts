// A message line as the firmware runs it (ELITE_RGB textline_init 0x466C8, message_text_next_token 0x46C78,
// textline_step 0x469D0, image_slide_init/step 0x4D24C/0x4D354), ported from sign_emu.py statement by statement.
// Each segment is drawn once into a 0x1414-wide text image, then slid over the line image (the panel). A line plays
// once: step() returns 4 at its end, and the attract loop then moves on to the next line.

import { blit, cdiv, half, img, s16, s32, type Img } from "./image.ts"
import { FirmwareHang, fontPickForText, fontPickLargest, type FwFont } from "./font.ts"
import { motionCopy, nativeDraw, nativeTextWidth, type EmojiSheet } from "./native.ts"

export const TEXT_IMG_W = 0x1414
const ESC = 0x1b, CR = 0x0d

/** image_slide_init / image_slide_step: a straight line in frames = |d| / speed * 40 (40 fps). */
export class Slide {
  fx: number
  fy: number
  tx: number
  ty: number
  step = 0
  frames: number
  dx100: number
  dy100: number

  constructor(x0: number, y0: number, x1: number, y1: number, speed: number) {
    this.fx = x0; this.fy = y0; this.tx = x1; this.ty = y1
    const dx = s16(s16(x1) - s16(x0)), dy = s16(s16(y1) - s16(y0))
    const d = Math.fround(dx * dx + dy * dy)                 // __floatsisf, then double
    const f = speed ? (Math.sqrt(d) / speed) * 40.0 : Infinity
    this.frames = f >= 2 ** 32 ? 0xffffffff : Math.trunc(f)  // __fixunsdfsi
    const n = s16(this.frames)
    this.dx100 = s16(cdiv(dx * 100, n))
    this.dy100 = s16(cdiv(dy * 100, n))
  }

  pos(): [number, number] {
    const st = s16(this.step)
    return [cdiv(st * this.dx100 + 50, 100) + this.fx, cdiv(st * this.dy100 + 50, 100) + this.fy]
  }

  /** One frame. True when done (that frame blits at `to` without clearing first). */
  run(src: Img, dst: Img): boolean {
    if (this.frames < this.step) {
      blit(src, dst, this.tx, this.ty)
      motionCopy(src, dst, this.tx)                          // ext_image_blit: the per-letter effects go along
      return true
    }
    const [x, y] = this.pos()
    dst.d.fill(0)
    blit(src, dst, x, y)
    motionCopy(src, dst, x)
    this.step++
    return false
  }
}

export class TextLine {
  table: Uint8Array
  fonts: FwFont[]
  W: number
  H: number
  r24: number
  img: Img
  textImg: Img
  lpos = new Array<number>(256).fill(0)
  stack = new Array<number>(256).fill(0)
  state = 0
  line: number
  depth = 0
  entryDir = 0
  exitDir = 0
  entrySpeed = 40
  exitSpeed = 40
  hold = 0
  fg = 0x7c00
  bg = 0
  proportional = 0
  boxX: number
  font: number
  text = new Uint8Array(0)
  pos: [number, number] = [0, 0]
  textRect = [0, 0, 0, 0]
  sl: Slide | null = null

  /** table: the message lines, 256 bytes each (shorter is padded with NUL). */
  /** the sign's native emoji (firmware with native_emoji.c): drawn wherever the text has their UTF-8 bytes */
  sheet?: EmojiSheet

  /** mode: textline_init's last argument, which the attract loop gives the line's outline style: font_pick_largest
   *  then picks for a box 2 px shorter (thick, thin: rect_grow 0,1,0,-1) or 1 px shorter (drop shadow), room for it */
  /** the fonts the firmware's automatic picks may choose (fonts.auto), when text names none */
  auto?: ReadonlySet<string>

  constructor(table: Uint8Array, fonts: FwFont[], W: number, H: number, line = 0, r24 = 0, boxX = 0, sheet?: EmojiSheet, mode = 0,
              auto?: ReadonlySet<string>) {
    this.sheet = sheet
    this.auto = auto
    this.table = new Uint8Array(256 * 256)
    this.table.set(table.subarray(0, 256 * 256))
    this.fonts = fonts; this.W = W; this.H = H; this.r24 = r24
    this.img = img(W, H)
    this.textImg = img(TEXT_IMG_W, H)
    this.line = line
    this.boxX = boxX
    const f = fontPickLargest(fonts, H - (mode === 1 || mode === 2 ? 2 : mode === 3 ? 1 : 0), auto)
    if (f === null) throw new Error("no fonts")
    this.font = f
  }

  get f(): FwFont {
    return this.fonts[this.font]
  }

  private t(p: number): number {
    return this.table[p] ?? 0
  }

  /** message_text_next_token */
  nextToken() {
    let r24 = this.r24
    let p = this.line * 256 + this.lpos[this.line]
    this.state = 0
    for (;;) {
      const b = this.t(p)
      p++
      this.lpos[this.line] = (this.lpos[this.line] + 1) & 0xffff
      if (b === 0) {                                         // end of line: pop an include, or done
        this.lpos[this.line] = 0
        this.depth = (this.depth - 1) & 0xff
        if (this.depth & 0x80) {
          this.state = 4
          return
        }
        this.line = this.stack[this.depth]
        p = this.line * 256 + this.lpos[this.line]
        continue
      }
      if (b !== ESC) {
        const text = [b]
        while (![CR, ESC, 0].includes(this.t(p)) && text.length < 256) text.push(this.t(p++))
        this.lpos[this.line] = (p - this.line * 256) & 0xffff
        this.text = Uint8Array.from(text)
        this.layoutAndSlide(r24)
        this.state = 1
        return
      }
      const cmd = this.t(p), a = p + 1
      this.lpos[this.line] = (this.lpos[this.line] + 1) & 0xffff
      if (cmd === 0x30 || cmd === 0x31) {                    // colours, big-endian RGB555
        const v = (this.t(a) << 8) | this.t(a + 1)
        if (cmd === 0x30) this.fg = v
        else this.bg = v
        p += 3
        this.lpos[this.line] += 2
      } else if (cmd >= 0x40 && cmd <= 0x44) {               // entry direction + speed
        this.entryDir = Math.min(cmd - 0x40, 3)
        this.entrySpeed = this.t(a)
        if (cmd === 0x44) r24 = 1
        p += 2
        this.lpos[this.line] += 1
      } else if (cmd >= 0x50 && cmd <= 0x54) {               // hold, exit direction + speed
        this.exitDir = Math.min(cmd - 0x50, 3)
        this.hold = this.t(a) << 2
        this.exitSpeed = this.t(a + 1)
        if (cmd === 0x54) r24 = 1
        p += 3
        this.lpos[this.line] += 2
      } else if (cmd === 0x60) {                             // font n, or 0xFF: proportional (centred)
        const n = this.t(a)
        if (n === 0xff) this.proportional = 1
        else if (n < this.fonts.length) {
          this.font = n
          this.proportional = 0
        }
        p += 2
        this.lpos[this.line] += 1
      } else if (cmd === 0x80) {                             // include line n
        this.stack[this.depth] = this.line
        this.depth = (this.depth + 1) & 0xff
        this.lpos[this.line] += 1
        this.line = this.t(a)
        p = this.line * 256 + this.lpos[this.line]
      } else p += 1                                          // unknown: skip ESC + command byte
    }
  }

  private layoutAndSlide(r24: number) {
    let f = this.f
    const ti = this.textImg, th = this.H, tiw = TEXT_IMG_W, lw = this.W, lh = this.H
    ti.d.fill(0)
    if (this.proportional === 0) {
      this.pos = [this.boxX, s32(((th - f.ch) >>> 0) >>> 1)]
      this.textRect = [0, 0, this.text.length * f.cw, f.ch]
    } else {                                                 // pick a font, then text_centre_in_rect
      const box: [number, number] = this.entryDir < 2 ? [tiw, th] : [this.W, this.H]
      const n = fontPickForText(this.fonts, this.text.subarray(0, 1), box[0], box[1], this.auto)
      if (n !== null) this.font = n
      f = this.f
      const w = Math.min(nativeTextWidth(f, this.text, this.sheet), tiw), h = Math.min(f.ch, th)
      this.textRect = [0, 0, w, h]
      this.pos = [half(tiw - w), half(th - h)]
    }
    this.textRect[2] = nativeDraw(f, ti, this.pos[0], this.pos[1], this.text, this.fg, this.bg, this.sheet)
    const px = s16(this.pos[0]), py = s16(this.pos[1])
    const tw = s16(this.textRect[2]), trh = s16(this.textRect[3])
    const cy = s16(((th - lh) >>> 0) >>> 1), cx = s16(((tiw - lw) >>> 0) >>> 1)
    const hold = this.hold, d = this.entryDir
    let sl: [number, number, number, number] | null
    if (d === 0) sl = [s16(lw - px), -cy, hold === 0 ? -s16(px + tw) : -cx, -cy]
    else if (d === 1) sl = [-s16(px + tw), -cy, hold === 0 ? s16(lw - px) : -cx, -cy]
    else if (d === 2) sl = [-cx, s16(lh - py), -cx, hold === 0 ? -s16(py + trh) : -cy]
    else if (d === 3 && !r24) sl = [-cx, -s16(lh), -cx, hold === 0 ? s16(lh - py) : -cy]
    else if (d === 3) sl = [0, -s16(lh), 0, hold === 0 ? s16(lh - py) : -cy]
    else sl = null
    if (sl) this.sl = new Slide(sl[0], sl[1], sl[2], sl[3], this.entrySpeed)
  }

  /** textline_step: one frame. 0 next segment, 1 slide in, 2 slide out, 3 hold, 4 done. */
  step(): number {
    const st = this.state
    if (st === 0) this.nextToken()
    else if (st === 1) {
      if (this.sl!.run(this.textImg, this.img)) this.state = this.hold ? 3 : 0
    } else if (st === 2) {
      if (this.sl!.run(this.textImg, this.img)) this.state = 0
    } else if (st === 3) {
      const d = this.hold
      this.hold = (d - 1) >>> 0
      if (d === 0) {
        const sl = this.sl!
        const tx = s16(sl.tx), ty = s16(sl.ty), px = s16(this.pos[0]), py = s16(this.pos[1])
        const tw = s16(this.textRect[2]), trh = s16(this.textRect[3])
        const e = this.exitDir
        if (e === 0) this.sl = new Slide(tx, ty, -s16(px + tw), ty, this.exitSpeed)
        else if (e === 1) this.sl = new Slide(tx, ty, s16(this.W - px), ty, this.exitSpeed)
        else if (e === 2) this.sl = new Slide(tx, ty, tx, -s16(py + trh), this.exitSpeed)
        else if (e === 3) this.sl = new Slide(tx, ty, tx, s16(this.H - py), this.exitSpeed)
        this.state = 2
      }
    }
    return this.state
  }
}

/** Run a line to its end (state 4) or maxFrames: the line image after each 40 fps frame, and the states (-1: the
 *  firmware hangs there). */
export function textlineFrames(table: Uint8Array, fonts: FwFont[], W: number, H: number, line = 0,
                               maxFrames = 2400, r24 = 0): { frames: Img[]; states: number[] } {
  const tl = new TextLine(table, fonts, W, H, line, r24)
  const frames: Img[] = [], states: number[] = []
  while (frames.length < maxFrames) {
    let st: number
    try {
      st = tl.step()
    } catch (e) {
      if (!(e instanceof FirmwareHang)) throw e
      states.push(-1)
      frames.push({ w: W, h: H, d: tl.img.d.slice() })
      break
    }
    frames.push({ w: W, h: H, d: tl.img.d.slice() })
    states.push(st)
    if (st === 4) break
  }
  return { frames, states }
}
