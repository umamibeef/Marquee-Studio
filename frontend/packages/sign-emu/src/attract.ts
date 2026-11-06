// The attract loop, as display_main_loop runs it (state 1 substate 0xc / state 2; ELITE_RGB 0x683c, decompile
// 4454-4552): the enabled message lines (cfg_attract_line_mode) with text, one after another, each played once by
// the text line; each frame the line image goes onto the panel through the line's effect word
// (cfg_attract_line_effect) and outline (cfg_attract_line_outline). A preview of what the sign shows.

import { img, s32, type Img } from "./image.ts"
import type { FwFont } from "./font.ts"
import { TextLine } from "./textline.ts"
import { Effects, outlineBlit } from "./effects.ts"
import type { EmojiSheet } from "./native.ts"
import type { AppInstance } from "./appvm.ts"

export interface AttractLine {
  /** the line's bytes (text and markup), at most 255 */
  data: Uint8Array
  /** FG<<16 | BG */
  effect: number
  /** 0 none, 1 thick, 2 thin, 3 drop shadow */
  outlineStyle?: number
  /** colour index 0..7 (colourIndexToRgb555) */
  outlineColour?: number
  /** the line's box x (textline_init's box): where a fixed-font segment starts in the text image */
  boxX?: number
  /** a programmable app owns the line (firmware/ext/apps.c): its buffer is the frame while the line is on */
  program?: AppInstance
}

/** One panel frame of an attract line: the frame buffer `frame` (not cleared: the firmware cycles 8 and draws over
 *  the oldest) gets the line image `lineImg`, drawn in font `f`. */
export function composeAttract(fx: Effects, frame: Img, lineImg: Img, f: FwFont, line: AttractLine) {
  const W = frame.w, H = frame.h
  const cellH = f.h >> 4                                     // layout 2 (fonteNN): the cell height
  const y = s32(((H - cellH) >>> 0) >>> 1)
  const band = [0, y, W, cellH]
  let word = line.effect >>> 0
  const style = line.outlineStyle ?? 0
  if (style) {                                               // (effects_outline_enabled: on)
    fx.blitEffect(img(W, H), frame, word, [0, 0, W, H], [0, 0])   // the background over the whole panel
    word = ((word & 0xffff0000) | 0x8000) >>> 0                    // the letters then leave it alone
    outlineBlit(lineImg, [...band], frame, [0, y], style, line.outlineColour ?? 0, [0, 0, W, H])
  }
  fx.blitEffect(lineImg, frame, word, band, [0, y])
}

/** Plays attract lines as the sign does: each enabled line with text once, then the next, round and round. Between
 *  two lines the firmware gives one frame to the attract animations (local_9c is cleared at the line's end, so the
 *  next frame runs attract_play_next_maybe before the next line starts): with no attractNN.flc loaded that frame is
 *  black. (With attract animations in the archive they would play there; not modelled.) Checked frame for frame
 *  against the firmware in the C++ simulator (test/firmware.test.ts). */
export class AttractPlayer {
  fonts: FwFont[]
  W: number
  H: number
  lines: (AttractLine | null)[]
  fx: Effects
  ring: Img[]
  k = 0
  cur = -1
  tl: TextLine | null = null
  gap = false                                                // the attract-animation frame between two lines
  table = new Uint8Array(256 * 17)
  sheet?: EmojiSheet
  /** frames the current line's app has run */
  appT = 0
  /** steps the current message line has run (the clock of its animated emoji: apps.c S.line_steps) */
  lineT = 0

  /** lines: up to 16, null for a line that is off. */
  /** the fonts the firmware's automatic picks may choose (fonts.auto) */
  auto?: ReadonlySet<string>

  constructor(fonts: FwFont[], W: number, H: number, lines: (AttractLine | null)[], fx = new Effects(), sheet?: EmojiSheet,
              auto?: ReadonlySet<string>) {
    this.sheet = sheet
    this.auto = auto
    this.fonts = fonts; this.W = W; this.H = H; this.lines = lines.slice(0, 16); this.fx = fx
    this.ring = Array.from({ length: 8 }, () => img(W, H))
    this.lines.forEach((l, i) => l && this.table.set(l.data.subarray(0, 255), 256 * i))
  }

  /** the line on screen (-1 none) */
  get line(): number {
    return this.cur
  }

  private next(): boolean {
    for (let k = 1; k <= 16; k++) {
      const i = (this.cur + k + 16) % 16
      const l = this.lines[i]
      if (l && (l.program || (l.data.length && l.data[0] !== 0))) {
        this.cur = i
        this.tl = new TextLine(this.table, this.fonts, this.W, this.H, i, 0, l.boxX ?? 0, this.sheet, l.outlineStyle ?? 0, this.auto)
        this.appT = 0
        this.lineT = 0
        return true
      }
    }
    this.cur = -1
    this.tl = null
    return false
  }

  /** One 40 fps frame: the panel after it (with no line to show, the attract animations: black here). */
  frame(): Img {
    const frame = this.ring[this.k++ % 8]
    if (this.gap || (!this.tl && !this.next())) {        // attract_play_next_maybe with no animations: black
      this.gap = false
      frame.d.fill(0)
      this.fx.frameCount++
      return { w: this.W, h: this.H, d: frame.d.slice() }
    }
    const tl = this.tl!
    const line = this.lines[this.cur]!
    if (line.program) {                                        // ext_textline_step / ext_frame_submit: the app's buffer
      const done = line.program.frame(this.appT++)
      frame.d.set(line.program.buf.d)
      this.fx.frameCount++
      if (done) {
        this.gap = true
        this.next()
      }
      return { w: this.W, h: this.H, d: frame.d.slice() }
    }
    this.fx.emojiT = this.lineT++
    const st = tl.step()
    composeAttract(this.fx, frame, tl.img, tl.f, line)
    this.fx.frameCount++
    if (st === 4) {
      this.gap = true
      this.next()
    }
    return { w: this.W, h: this.H, d: frame.d.slice() }
  }
}

