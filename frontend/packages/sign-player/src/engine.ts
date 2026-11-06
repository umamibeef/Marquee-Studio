// The LED panel player, ported from the classic studio (studio/static/index.html, class SignPlayer).
// It draws RGB555 frames (from /api/preview and friends) as round LEDs on a dark stage, with the afterglow:
// a pixel turns on at once but only fades towards a darker value. The fade is continuous in time and applied at
// the screen's refresh rate: after dt ms a level keeps persist^(dt / frame ms), so `persist` still means "kept per
// sign frame"; shown = max(sign frame, faded) per channel, in floating point. Sign frames advance on their own
// clock (frames.ms, 25 ms = 40 fps); the canvas redraws on requestAnimationFrame.

export interface Frames {
  w: number
  h: number
  nframes: number
  /** milliseconds per frame (the sign's 40 fps panel frames: 25) */
  ms?: number
  /** base64, RGB555 little-endian, nframes * h * w pixels */
  data: string
}

export interface LookOptions {
  /** gamma applied to each 5-bit channel (1 = linear) */
  gamma: number
  /** 0..1 */
  brightness: number
  /** round LED dots (true) or square pixels */
  dots: boolean
  /** afterglow: fraction of a level kept per sign frame (0 = off) */
  persist: number
  /** show unlit LEDs faintly, like a real panel */
  unlit: boolean
}

export const DEFAULT_LOOK: LookOptions = { gamma: 1, brightness: 1, dots: true, persist: 0.3, unlit: true }

function buildLut(look: LookOptions) {
  const f = (v: number) => {
    const x = Math.pow(v / 31, look.gamma) * look.brightness
    return Math.max(0, Math.min(255, Math.round(x * 255)))
  }
  const flut = new Uint8Array(32 * 32 + 1) // 1/32-step levels, for the smooth afterglow
  for (let i = 0; i < flut.length; i++) flut[i] = f(Math.min(31, i / 32))
  return flut
}

export function decodeFrames(f: Frames): Uint8Array {
  const raw = atob(f.data)
  const b = new Uint8Array(raw.length)
  for (let k = 0; k < raw.length; k++) b[k] = raw.charCodeAt(k)
  return b
}

export class PanelPlayer {
  private ctx: CanvasRenderingContext2D
  private glowCtx: CanvasRenderingContext2D | null
  private bytes: Uint8Array | null = null
  private w = 0
  private h = 0
  private ms = 25
  private n = 1
  private vis: Float32Array | null = null
  private flut: Uint8Array
  private raf = 0
  private last: number | null = null
  private acc = 0
  private pitch = 1
  private tmp: OffscreenCanvas | null = null
  i = 0
  playing = false
  onFrame: ((i: number) => void) | null = null
  look: LookOptions

  constructor(
    private canvas: HTMLCanvasElement,
    private glow: HTMLCanvasElement | null,
    look: LookOptions,
  ) {
    this.ctx = canvas.getContext("2d")!
    this.glowCtx = glow ? glow.getContext("2d") : null
    this.look = look
    this.flut = buildLut(look)
  }

  get frameCount() {
    return this.n
  }
  get size() {
    return { w: this.w, h: this.h }
  }

  setLook(look: LookOptions) {
    this.look = look
    this.flut = buildLut(look)
    this.draw()
  }

  load(f: Frames, keepPosition = false) {
    this.bytes = decodeFrames(f)
    const sameSize = this.w === f.w && this.h === f.h
    this.w = f.w
    this.h = f.h
    this.ms = f.ms || 60
    this.n = Math.max(1, f.nframes)
    if (!keepPosition || !sameSize) this.i = 0
    else this.i = this.i % this.n
    this.vis = null
    this.step()
  }

  /** CSS pixel width the panel should take; the pitch is a whole number of device pixels per LED. */
  layout(cssWidth: number) {
    if (!this.w) return
    const dpr = window.devicePixelRatio || 1
    // A whole number of device pixels per LED, at every size: every LED is drawn alike (a fractional pitch makes some
    // LEDs a pixel wider than others, which distorts thin pixel-font strokes). Below one pixel per LED, shrink.
    const exact = (cssWidth * dpr) / this.w
    this.pitch = exact >= 1 ? Math.floor(exact) : Math.max(0.25, exact)
    const cw = Math.round(this.w * this.pitch)
    const ch = Math.round(this.h * this.pitch)
    if (this.canvas.width !== cw || this.canvas.height !== ch) {
      this.canvas.width = cw
      this.canvas.height = ch
    }
    this.canvas.style.width = `${cw / dpr}px`
    this.canvas.style.height = `${ch / dpr}px`
    if (this.glow) {
      if (this.glow.width !== this.w || this.glow.height !== this.h) {
        this.glow.width = this.w
        this.glow.height = this.h
      }
      this.glow.style.width = `${cw / dpr}px`
      this.glow.style.height = `${ch / dpr}px`
      this.glow.style.filter = `blur(${Math.max(1, (this.pitch / dpr) * 0.7)}px)`
    }
    this.draw()
  }

  private step() {
    this.fade(Infinity)
    this.apply()
  }

  private fade(dt: number) {
    const n = this.w * this.h * 3
    if (!this.vis || this.vis.length !== n) {
      this.vis = new Float32Array(n)
      return
    }
    const p = this.look.persist
    const k = p > 0 && isFinite(dt) ? Math.pow(p, dt / this.ms) : 0
    const vis = this.vis
    for (let o = 0; o < n; o++) vis[o] *= k
  }

  private apply() {
    if (!this.bytes || !this.vis) return
    const n = this.w * this.h
    const base = this.i * n * 2
    const b = this.bytes
    const vis = this.vis
    for (let p = 0; p < n; p++) {
      const v = b[base + p * 2] | (b[base + p * 2 + 1] << 8)
      const o = p * 3
      const r = (v >> 10) & 31
      const g = (v >> 5) & 31
      const bl = v & 31
      if (r > vis[o]) vis[o] = r
      if (g > vis[o + 1]) vis[o + 1] = g
      if (bl > vis[o + 2]) vis[o + 2] = bl
    }
  }

  draw() {
    if (!this.bytes || !this.vis) return
    const { w, h, pitch, flut, vis } = this
    const ctx = this.ctx
    const dots = this.look.dots && pitch >= 3                  // round LEDs from 3 device pixels per LED
    let img: ImageData | null = null
    if (this.glowCtx) img = this.glowCtx.createImageData(w, h)
    if (dots) {
      ctx.fillStyle = "#050506"
      ctx.fillRect(0, 0, this.canvas.width, this.canvas.height)
      const rad = pitch * 0.4
      if (this.look.unlit) {
        ctx.fillStyle = "#16161a"
        ctx.beginPath()
        for (let y = 0; y < h; y++)
          for (let x = 0; x < w; x++) {
            const cx = x * pitch + pitch / 2
            const cy = y * pitch + pitch / 2
            ctx.moveTo(cx + rad, cy)
            ctx.arc(cx, cy, rad, 0, 6.2832)
          }
        ctx.fill()
      }
      for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
          const p = y * w + x
          const r = flut[Math.round(vis[p * 3] * 32)]
          const g = flut[Math.round(vis[p * 3 + 1] * 32)]
          const bl = flut[Math.round(vis[p * 3 + 2] * 32)]
          if (img) {
            img.data[p * 4] = r
            img.data[p * 4 + 1] = g
            img.data[p * 4 + 2] = bl
            img.data[p * 4 + 3] = 255
          }
          if (r + g + bl < 3) continue                         // off: the unlit dot drawn above
          // a lit LED is never darker than an unlit one (#16161a)
          ctx.fillStyle = this.look.unlit ? `rgb(${Math.max(r, 22)},${Math.max(g, 22)},${Math.max(bl, 26)})` : `rgb(${r},${g},${bl})`
          ctx.beginPath()
          ctx.arc(x * pitch + pitch / 2, y * pitch + pitch / 2, rad, 0, 6.2832)
          ctx.fill()
        }
    } else {
      const full = ctx.createImageData(w, h)
      for (let p = 0; p < w * h; p++) {
        const lo = this.look.unlit ? 22 : 0, loB = this.look.unlit ? 26 : 0   // never darker than an unlit LED
        full.data[p * 4] = Math.max(flut[Math.round(vis[p * 3] * 32)], lo)
        full.data[p * 4 + 1] = Math.max(flut[Math.round(vis[p * 3 + 1] * 32)], lo)
        full.data[p * 4 + 2] = Math.max(flut[Math.round(vis[p * 3 + 2] * 32)], loB)
        full.data[p * 4 + 3] = 255
      }

      // scale up crisply: draw the 1:1 image then stretch with smoothing off
      ctx.imageSmoothingEnabled = false
      if (!this.tmp || this.tmp.width !== w || this.tmp.height !== h) this.tmp = new OffscreenCanvas(w, h)
      this.tmp.getContext("2d")!.putImageData(full, 0, 0)
      ctx.drawImage(this.tmp, 0, 0, this.canvas.width, this.canvas.height)
      img = this.glowCtx ? full : null
    }
    if (img && this.glowCtx) this.glowCtx.putImageData(img, 0, 0)
  }

  play() {
    this.playing = true
    this.last = null
    this.acc = 0
    this.draw()
    if (this.n > 1 || this.look.persist > 0) {
      if (!this.raf) this.raf = requestAnimationFrame((t) => this.tick(t))
    }
  }

  private tick(t: number) {
    this.raf = 0
    if (!this.playing) return
    const dt = this.last == null ? 0 : Math.min(250, t - this.last)
    this.last = t
    this.acc += dt
    let moved = false
    while (this.acc >= this.ms) {
      this.acc -= this.ms
      this.i = (this.i + 1) % this.n
      moved = true
    }
    const r = this.canvas.getBoundingClientRect()
    const seen = r.bottom > 0 && r.top < window.innerHeight && r.width > 0
    if (seen && (moved || this.look.persist > 0)) {
      this.fade(dt)
      this.apply()
      this.draw()
    } else if (!seen && moved) this.vis = null
    if (moved && this.onFrame) this.onFrame(this.i)
    if (this.n > 1) this.raf = requestAnimationFrame((t2) => this.tick(t2))
  }

  pause() {
    this.playing = false
    if (this.raf) cancelAnimationFrame(this.raf)
    this.raf = 0
  }

  seek(i: number) {
    this.i = ((i % this.n) + this.n) % this.n
    this.vis = null
    this.step()
    this.draw()
    if (this.onFrame) this.onFrame(this.i)
  }

  dispose() {
    this.pause()
    this.onFrame = null
  }

  toBlob(): Promise<Blob | null> {
    return new Promise((res) => this.canvas.toBlob(res, "image/png"))
  }
}
