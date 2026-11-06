// Programmable apps (docs/APPS.md) in the Studio: an MQAP bundle (tools/mkapp.py) run by the eBPF interpreter
// (ebpf.ts) with the helpers of apps/include/sign.h, exactly as our firmware runs it (firmware/ext/apps.c): the same
// text mask (the firmware's 0x1414-wide text image), the same effect blit, outline and emoji drawing, the same
// strftime. test/appvm_firmware.test.ts compares the frames with the firmware's in the simulator.
import { Effects, outlineBlit } from "./effects.ts"
import { fontPickLargest, type FwFont } from "./font.ts"
import { img, type Img } from "./image.ts"
import { motionReset, nativeDraw, nativeTextWidth, type EmojiSheet } from "./native.ts"
import { EbpfVm, ebpfCheck, EBPF_STACK_VA, type EbpfHelper } from "./ebpf.ts"
import { TEXT_IMG_W } from "./textline.ts"

export const APP_DATA_VA = 0x20000000
export const APP_BUDGET = 200_000                     // eBPF instructions per init() / frame() call
export const MASK_X = 2048                            // where x = 0 falls in the text mask

export interface AppAsset { name: string; w: number; h: number; frames: number; ms: number; pixels: Uint16Array }

export interface AppBundle {
  code: Uint8Array
  data: Uint8Array
  bss: number
  init: number
  frame: number
  settings: Map<string, Uint8Array>
  assets: AppAsset[]
  manifest: Record<string, unknown>
}

async function inflate(z: Uint8Array): Promise<Uint8Array> {
  const ds = new DecompressionStream("deflate")
  return new Uint8Array(await new Response(new Blob([z as BlobPart]).stream().pipeThrough(ds)).arrayBuffer())
}

/** parse an MQAP1 bundle (its assets unpacked, as the firmware does at load) */
export async function parseBundle(b: Uint8Array): Promise<AppBundle> {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength)
  if (dv.getUint32(0) !== 0x4d514150) throw new Error("not an app bundle")
  if (dv.getUint16(4) !== 1) throw new Error(`app ABI ${dv.getUint16(4)}: this Studio knows 1`)
  const [codeLen, dataLen, bss, init, frame, stLen, asLen, manLen] = Array.from({ length: 8 }, (_, k) => dv.getUint32(8 + 4 * k))
  let o = 40
  const code = b.subarray(o, (o += codeLen))
  const data = b.subarray(o, (o += dataLen))
  const st = b.subarray(o, (o += stLen))
  const as = b.subarray(o, (o += asLen))
  const man = b.subarray(o, (o += manLen))
  const settings = new Map<string, Uint8Array>()
  for (let i = 0; i < st.length; ) {
    let j = i
    while (j < st.length && st[j]) j++
    const pair = st.subarray(i, j), eq = pair.indexOf(0x3d)
    if (eq > 0) settings.set(new TextDecoder("latin1").decode(pair.subarray(0, eq)), pair.subarray(eq + 1))
    i = j + 1
  }
  const assets: AppAsset[] = []
  if (as.length >= 2) {
    const av = new DataView(as.buffer, as.byteOffset, as.byteLength)
    let p = 2
    for (let n = av.getUint16(0), k = 0; k < n; k++) {
      const nl = as[p++], name = new TextDecoder().decode(as.subarray(p, (p += nl)))
      const w = av.getUint16(p), h = av.getUint16(p + 2), frames = av.getUint16(p + 4), ms = av.getUint16(p + 6), zl = av.getUint32(p + 8)
      p += 12
      const raw = await inflate(as.subarray(p, (p += zl)))
      const pixels = new Uint16Array(w * h * frames)
      for (let i = 0; i < pixels.length; i++) pixels[i] = (raw[2 * i] << 8) | raw[2 * i + 1]
      if (assets.length < 8) assets.push({ name, w, h, frames, ms, pixels })
    }
  }
  return { code, data, bss, init, frame, settings, assets, manifest: man.length ? JSON.parse(new TextDecoder().decode(man)) : {} }
}

/** what the sign gives an app: its fonts (in the firmware's order, with their names), emoji sheet, effect slots */
export interface AppHost {
  fonts: FwFont[]
  fontNames: string[]
  sheet?: EmojiSheet
  fx: Effects
  /** the sign's wall-clock time in seconds (local time, sign seconds), or -1 when it is not set */
  now: () => number
  /** the fonts the sign picks from by itself (fonts.auto: firmware/auto_fonts.txt) */
  auto?: ReadonlySet<string>
}

/** a value pushed to the app (weather...), with the sign time it arrived at */
export interface AppDatum { value: Uint8Array; at: number }

// ---- strftime: firmware/ext/apps.c strftime_ (sign seconds are local wall-clock seconds) ----
const DAYS = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
const MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"]

function breakdown(t: number) {
  const days = Math.floor(t / 86400), rem = t - days * 86400
  const hour = Math.floor(rem / 3600), min = Math.floor(rem / 60) % 60, sec = rem % 60
  const wday = (((days + 4) % 7) + 7) % 7
  const z = days + 719468
  const era = Math.floor(z / 146097)
  const doe = z - era * 146097
  const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365)
  const y = yoe + era * 400
  const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100))
  const mp = Math.floor((5 * doy + 2) / 153)
  const mday = doy - Math.floor((153 * mp + 2) / 5) + 1
  const mon = (mp < 10 ? mp + 3 : mp - 9) - 1
  const year = y + (mon < 2 ? 1 : 0)
  const before = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]
  const leap = (year % 4 === 0 && year % 100 !== 0) || year % 400 === 0
  return { year, mon, mday, hour, min, sec, wday, yday: before[mon] + mday - 1 + (leap && mon > 1 ? 1 : 0) }
}

/** strftime_ over bytes (fmt as stored in the app's memory), up to max output bytes */
export function signStrftime(fmt: Uint8Array, t: number, max: number): number[] {
  const tm = breakdown(t), o: number[] = []
  const put = (c: number) => { if (o.length < max) o.push(c) }
  const puts = (s: string) => { for (const ch of s) put(ch.charCodeAt(0)) }
  const putn = (v: number, width: number, pad: string) => {
    const s = String(Math.abs(v))
    if (v < 0) put(0x2d)
    for (let k = s.length; k < width; k++) puts(pad)
    puts(s)
  }
  for (let i = 0; i < fmt.length && fmt[i]; i++) {
    if (fmt[i] !== 0x25 || i + 1 >= fmt.length || !fmt[i + 1]) {
      put(fmt[i])
      continue
    }
    const c = String.fromCharCode(fmt[++i]), h12 = tm.hour % 12 ? tm.hour % 12 : 12
    switch (c) {
      case "H": putn(tm.hour, 2, "0"); break
      case "M": putn(tm.min, 2, "0"); break
      case "S": putn(tm.sec, 2, "0"); break
      case "I": putn(h12, 2, "0"); break
      case "l": putn(h12, 1, "0"); break
      case "p": puts(tm.hour < 12 ? "AM" : "PM"); break
      case "P": puts(tm.hour < 12 ? "am" : "pm"); break
      case "k": putn(tm.hour, 2, " "); break
      case "j": putn(tm.yday + 1, 3, "0"); break
      case "u": putn(tm.wday ? tm.wday : 7, 1, "0"); break
      case "w": putn(tm.wday, 1, "0"); break
      case "F": putn(tm.year, 1, "0"); put(0x2d); putn(tm.mon + 1, 2, "0"); put(0x2d); putn(tm.mday, 2, "0"); break
      case "D": putn(tm.mon + 1, 2, "0"); put(0x2f); putn(tm.mday, 2, "0"); put(0x2f); putn(tm.year % 100, 2, "0"); break
      case "a": puts(DAYS[tm.wday].slice(0, 3)); break
      case "A": puts(DAYS[tm.wday]); break
      case "b": puts(MONTHS[tm.mon].slice(0, 3)); break
      case "B": puts(MONTHS[tm.mon]); break
      case "d": putn(tm.mday, 2, "0"); break
      case "e": putn(tm.mday, 2, " "); break
      case "m": putn(tm.mon + 1, 2, "0"); break
      case "y": putn(tm.year % 100, 2, "0"); break
      case "Y": putn(tm.year, 1, "0"); break
      case "T": putn(tm.hour, 2, "0"); put(0x3a); putn(tm.min, 2, "0"); put(0x3a); putn(tm.sec, 2, "0"); break
      case "R": putn(tm.hour, 2, "0"); put(0x3a); putn(tm.min, 2, "0"); break
      case "%": put(0x25); break
      default: put(0x25); put(c.charCodeAt(0))
    }
  }
  return o
}

function parseInt32(v: Uint8Array | undefined, dflt: number): number {
  if (!v || !v.length) return dflt
  let i = 0, neg = false, n = 0
  if (v[0] === 0x2d) { neg = true; i = 1 }
  if (i >= v.length) return dflt
  for (; i < v.length; i++) {
    if (v[i] < 0x30 || v[i] > 0x39) return dflt
    n = (n * 10 + (v[i] - 0x30)) | 0
  }
  return neg ? -n | 0 : n
}

const i32 = (v: number) => BigInt.asUintN(64, BigInt(v | 0))

/** One installed app: its own memory (globals kept between frames), its buffer, its pushed data. */
export class AppInstance {
  b: AppBundle
  sign: AppHost
  W: number
  H: number
  buf: Img
  mask: Img
  omask: Img
  mem: Uint8Array
  vm: EbpfVm
  data = new Map<string, AppDatum>()
  log: string[] = []
  error = ""

  constructor(bundle: AppBundle, sign: AppHost, W: number, H: number) {
    this.b = bundle; this.sign = sign; this.W = W; this.H = H
    this.buf = img(W, H)
    this.mask = img(TEXT_IMG_W, H)
    this.omask = img(W, H)
    this.mem = new Uint8Array(bundle.data.length + bundle.bss)
    this.mem.set(bundle.data)
    const bad = ebpfCheck(bundle.code, bundle.code.length >> 3, HELPERS.length)
    this.vm = new EbpfVm(bundle.code, HELPERS.map((h) => (h ? (vm: EbpfVm, a: bigint, b: bigint, c: bigint, d: bigint, e: bigint) => h(this, vm, a, b, c, d, e) : null)) as EbpfHelper[])
    this.vm.region(EBPF_STACK_VA, this.vm.stack.length, this.vm.stack, true)
    this.vm.region(APP_DATA_VA, this.mem.length, this.mem, true)
    this.vm.budget = APP_BUDGET
    if (bad) this.error = `invalid instruction ${bad - 1}`
  }

  private call(pc: number, args: bigint[]): bigint | null {
    if (this.error) return null
    const r = this.vm.run(pc, args)
    if (this.vm.error) {
      this.error = ["", "invalid instruction", "memory access", "out of time", "no such helper", "calls too deep", "jump outside", "stopped"][this.vm.error] + ` at pc ${this.vm.errorPc}`
      return null
    }
    return r
  }

  /** init(): false when it failed */
  init(): boolean {
    const r = this.call(this.b.init, [])
    if (r !== null && r !== 0n) this.error = `init() returned ${BigInt.asIntN(32, r)}`
    return !this.error
  }

  /** frame(t): true when the app's turn is over (or it failed) */
  frame(t: number): boolean {
    this.omask.d.fill(0)
    this.sign.fx.emojiT = t >>> 0
    const r = this.call(this.b.frame, [BigInt(t >>> 0)])
    if (r === null) return true
    return (r & 0xffffffffn) !== 0n
  }

  // ---- memory access for helpers ----
  str(va: bigint): Uint8Array | null {
    for (let n = 1; n <= 256; n++) {
      const m = this.vm.mem(va, n, false)
      if (!m) break
      if (m[0][m[1] + n - 1] === 0) return m[0].subarray(m[1], m[1] + n - 1)
    }
    this.vm.stop()
    return null
  }

  out(va: bigint, len: number, bytes: ArrayLike<number>): number {
    if (len <= 0) return -1
    const m = this.vm.mem(va, len, true)
    if (!m) { this.vm.stop(); return -1 }
    const n = Math.min(bytes.length, len - 1)
    for (let k = 0; k < n; k++) m[0][m[1] + k] = bytes[k]
    m[0][m[1] + n] = 0
    return n
  }

  font(n: number): FwFont | null {
    return n >= 0 && n < this.sign.fonts.length ? this.sign.fonts[n] : null
  }

  /** paint the mask's window [MASK_X, 0, W, H] onto the buffer with paint (the firmware's effect blit), then clear
   *  the mask columns [x0, x1) */
  paint(paint: number, x0: number, x1: number) {
    const word = ((paint & 0xffff) << 16 | 0x8000) >>> 0
    this.sign.fx.blitEffect(this.mask, this.buf, word, [MASK_X, 0, this.W, this.H], [0, 0])
    motionReset(this.mask)                                     // the text's per-letter effects end with it
    const m = this.mask, a = Math.max(0, x0), b = Math.min(m.w, x1)
    for (let y = 0; y < m.h; y++) {
      for (let x = Math.max(a, MASK_X); x < Math.min(b, MASK_X + this.W); x++) if (m.d[y * m.w + x] && !(m.d[y * m.w + x] & 0x8000)) this.omask.d[y * this.W + x - MASK_X] = 0x7fff   // letters, not emoji
      m.d.fill(0, y * m.w + a, y * m.w + b)
    }
  }
}

type Helper = ((app: AppInstance, vm: EbpfVm, a: bigint, b: bigint, c: bigint, d: bigint, e: bigint) => bigint) | null
const n = (v: bigint) => Number(BigInt.asIntN(32, v))

/** apps/include/sign.h, helper n at HELPERS[n] */
export const HELPERS: Helper[] = [
  null,
  /* 1 width */ (app) => i32(app.W),
  /* 2 height */ (app) => i32(app.H),
  /* 3 clear */ (app, _vm, p) => { rectHelper(app, 0, 0, app.W, app.H, n(p)); return 0n },
  /* 4 rect */ (app, _vm, x, y, w, h, p) => { rectHelper(app, n(x), n(y), n(w), n(h), n(p)); return 0n },
  /* 5 pixel */ (app, _vm, x, y, c) => {
    const X = n(x), Y = n(y)
    if (X >= 0 && X < app.W && Y >= 0 && Y < app.H) app.buf.d[Y * app.W + X] = n(c) & 0x7fff
    return 0n
  },
  /* 6 text */ (app, _vm, f, x, y, p, s) => {
    const ft = app.font(n(f)), str = app.str(s)
    if (!ft || !str) return 0n
    const X = n(x), Y = n(y), w = nativeTextWidth(ft, str, app.sign.sheet)
    if (X + MASK_X < 0 || X + MASK_X + w > TEXT_IMG_W || Y <= -ft.ch || Y >= app.H) return i32(w)
    const drawn = nativeDraw(ft, app.mask, MASK_X + X, Y, str, 0x7fff, 0, app.sign.sheet)
    app.paint(n(p), MASK_X + X, MASK_X + X + drawn + 1)
    return i32(drawn)
  },
  /* 7 text_width */ (app, _vm, f, s) => {
    const ft = app.font(n(f)), str = app.str(s)
    return ft && str ? i32(nativeTextWidth(ft, str, app.sign.sheet)) : 0n
  },
  /* 8 font_height */ (app, _vm, f) => { const ft = app.font(n(f)); return i32(ft ? ft.ch : -1) },
  /* 9 font_find */ (app, _vm, s) => {
    const str = app.str(s)
    return i32(str ? app.sign.fontNames.indexOf(new TextDecoder("latin1").decode(str)) : -1)
  },
  /* 10 image */ (app, _vm, x, y, a, fr) => {
    const as = app.b.assets[n(a)]
    if (!as) return 0n
    const f = ((n(fr) % as.frames) + as.frames) % as.frames, X = n(x), Y = n(y)
    for (let yy = 0; yy < as.h; yy++)
      for (let xx = 0; xx < as.w; xx++) {
        const v = as.pixels[(f * as.h + yy) * as.w + xx], dx = X + xx, dy = Y + yy
        if (!(v & 0x8000) && dx >= 0 && dx < app.W && dy >= 0 && dy < app.H) app.buf.d[dy * app.W + dx] = v
      }
    return 0n
  },
  /* 11 asset_find */ (app, _vm, s) => {
    const str = app.str(s)
    return i32(str ? app.b.assets.findIndex((a) => a.name === new TextDecoder().decode(str)) : -1)
  },
  /* 12 asset_info */ (app, _vm, a, what) => {
    const as = app.b.assets[n(a)]
    return i32(!as ? -1 : [as.w, as.h, as.frames, as.ms][n(what)] ?? -1)
  },
  /* 13 now */ (app) => BigInt.asUintN(64, BigInt(app.sign.now())),
  /* 14 fmt_time */ (app, _vm, buf, len, fmt, t) => {
    const f = app.str(fmt)
    if (!f) return 0n
    return i32(app.out(buf, n(len), signStrftime(f, Number(BigInt.asIntN(64, t)), 255)))
  },
  /* 15 setting_int */ (app, _vm, k, d) => {
    const key = app.str(k)
    return i32(key ? parseInt32(app.b.settings.get(new TextDecoder("latin1").decode(key)), n(d)) : n(d))
  },
  /* 16 setting_str */ (app, _vm, k, buf, len) => {
    const key = app.str(k)
    const v = key ? app.b.settings.get(new TextDecoder("latin1").decode(key)) : undefined
    return i32(v === undefined ? -1 : app.out(buf, n(len), v))
  },
  /* 17 data_int */ (app, _vm, k, d) => {
    const key = app.str(k)
    return i32(key ? parseInt32(app.data.get(new TextDecoder("latin1").decode(key))?.value, n(d)) : n(d))
  },
  /* 18 data_str */ (app, _vm, k, buf, len) => {
    const key = app.str(k)
    const v = key ? app.data.get(new TextDecoder("latin1").decode(key)) : undefined
    return i32(v === undefined ? -1 : app.out(buf, n(len), v.value))
  },
  /* 19 data_age */ (app, _vm, k) => {
    const key = app.str(k)
    const v = key ? app.data.get(new TextDecoder("latin1").decode(key)) : undefined
    const now = app.sign.now()
    return i32(v === undefined || now < 0 ? -1 : Math.max(0, now - v.at))
  },
  /* 20 outline */ (app, _vm, style, colour) => {
    const st = n(style)
    if (st >= 1 && st <= 3) outlineBlit(app.omask, [0, 0, app.W, app.H], app.buf, [0, 0], st, n(colour) & 7, [0, 0, app.W, app.H])
    return 0n
  },
  /* 21 print */ (app, _vm, s) => { const str = app.str(s); if (str) app.log.push(new TextDecoder("latin1").decode(str)); return 0n },
  /* 22 font_auto */ (app, _vm, h) => i32(fontPickLargest(app.sign.fonts, n(h), app.sign.auto) ?? -1),
]

function rectHelper(app: AppInstance, x: number, y: number, w: number, h: number, p: number) {
  const x0 = Math.max(0, x), y0 = Math.max(0, y), x1 = Math.min(app.W, x + w), y1 = Math.min(app.H, y + h)
  if (x1 <= x0 || y1 <= y0) return
  for (let yy = y0; yy < y1; yy++) app.mask.d.fill(0x7fff, yy * app.mask.w + MASK_X + x0, yy * app.mask.w + MASK_X + x1)
  app.paint(p, MASK_X + x0, MASK_X + x1)
  for (let yy = y0; yy < y1; yy++) app.omask.d.fill(0, yy * app.W + x0, yy * app.W + x1)   // rects are not text
}
