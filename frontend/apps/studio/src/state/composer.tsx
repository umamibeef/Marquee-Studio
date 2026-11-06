// The message being composed (the Compose page), kept across pages and reloads (localStorage).
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react"

export type Motion = "scroll" | "static" | "flash"

export interface Composer {
  text: string
  /** per-character effects, aligned with text (UTF-16 units) */
  fx?: CharFx[]
  font: string
  motion: Motion
  /** pixels per panel frame (scroll) */
  textRate: number
  flashOnMs: number
  flashOffMs: number
  /** a palette colour name, or an animation file name (any .flc in the content folder) */
  fill: string
  /** a palette colour name ("Off" = black), an animation file, or a special effect name */
  background: string
  outline: string
  outlineStyle: string
  value: string
  amountMode: number
  amountDecimals: number
  amountPrefix: string
}

export const DEFAULT_COMPOSER: Composer = {
  text: "UMAMI MARQUEE",
  font: "fonte04.png",
  motion: "scroll",
  textRate: 2,
  flashOnMs: 500,
  flashOffMs: 500,
  fill: "Gold",
  background: "Off",
  outline: "Off",
  outlineStyle: "Thick",
  value: "",
  amountMode: 0,
  amountDecimals: 2,
  amountPrefix: "$",
}

/** Safe flash defaults: a 1 s cycle (WCAG 2.3.1 allows no more than 3 flashes a second). */
export const SAFE_FLASH = { on: 500, off: 500 }

export const isAnimation = (v: string) => v.toLowerCase().endsWith(".flc")

/** Per-character motion (firmware/ext/native_emoji.c, in-text command 0x1F m). */
export const MOTIONS = [
  { id: 0, name: "None" },
  { id: 1, name: "Wave" },
  { id: 2, name: "Bounce" },
  { id: 3, name: "Shake" },
  { id: 4, name: "Blink" },
] as const

/** a character's own effects: motion, fill (an effect word; null: the line's), on/off time in tenths (null: always) */
export interface CharFx { m: number; f: number | null; o: [number, number] | null }
export const NO_FX: CharFx = { m: 0, f: null, o: null }

/** per-character effects after text changed from old to now: the characters kept keep theirs; inserted ones take
 *  the effects of the character before them */
export function realignFx(old: string, fx: CharFx[] | undefined, now: string): CharFx[] {
  const f = Array.from({ length: old.length }, (_, i) => fx?.[i] ?? NO_FX)
  let a = 0
  while (a < old.length && a < now.length && old[a] === now[a]) a++
  let b = 0
  while (b < old.length - a && b < now.length - a && old[old.length - 1 - b] === now[now.length - 1 - b]) b++
  const inherit = a > 0 ? f[a - 1] : NO_FX
  return [...f.slice(0, a), ...Array(now.length - a - b).fill(inherit), ...f.slice(old.length - b)]
}

const hex = (v: number, n: number) => v.toString(16).toUpperCase().padStart(n, "0")
/** The text as the sign gets it: an in-text effect command (0x1F ...) wherever a character's effects change */
export function withEffects(text: string, fx: CharFx[] | undefined): string {
  let out = ""
  let cur: CharFx = NO_FX
  for (let i = 0; i < text.length; i++) {
    const e = fx?.[i] ?? NO_FX
    const low = i > 0 && text.charCodeAt(i) >= 0xdc00 && text.charCodeAt(i) <= 0xdfff   // 2nd half of a pair
    if (!low) {
      const oe = e.o ? `${e.o[0]},${e.o[1]}` : "", oc = cur.o ? `${cur.o[0]},${cur.o[1]}` : ""
      if (oe !== oc) out += "\x1fo" + (e.o ? hex(e.o[0] & 0xff, 2) + hex(e.o[1] & 0xff, 2) : "0000")
      if (e.m !== cur.m) out += "\x1fm" + e.m
      if (e.f !== cur.f) out += "\x1ff" + (e.f === null ? "-" : hex(e.f & 0xffff, 4))
      cur = e
    }
    out += text[i]
  }
  return out
}

/** The per-character motion names used in the readable style tags (index = motion id). */
const MOTION_TAG = ["", "wave", "bounce", "shake", "blink"] as const
const MOTION_ID: Record<string, number> = { none: 0, wave: 1, bounce: 2, shake: 3, blink: 4 }

/** The text with each character's effects as readable square-bracket tags (no closing slashes): a tag sets the state
 *  for the characters after it, exactly as the sign's 0x1F commands do. [wave]/[bounce]/[shake]/[blink] set motion
 *  and [none] clears it; [fill:HHHH] / [fill:none] set/clear the fill; [flash:on,off] / [flash:none] the on/off time.
 *  Round-trips with fromStyleTags. */
export function toStyleTags(text: string, fx: CharFx[] | undefined): string {
  let out = ""
  let cur: CharFx = NO_FX
  for (let i = 0; i < text.length; i++) {
    const e = fx?.[i] ?? NO_FX
    const low = i > 0 && text.charCodeAt(i) >= 0xdc00 && text.charCodeAt(i) <= 0xdfff
    if (!low) {
      if (e.m !== cur.m) out += e.m ? `[${MOTION_TAG[e.m]}]` : "[none]"
      if (e.f !== cur.f) out += e.f === null ? "[fill:none]" : `[fill:${hex(e.f & 0xffff, 4)}]`
      const oe = e.o ? `${e.o[0]},${e.o[1]}` : "", oc = cur.o ? `${cur.o[0]},${cur.o[1]}` : ""
      if (oe !== oc) out += e.o ? `[flash:${e.o[0]},${e.o[1]}]` : "[flash:none]"
      cur = e
    }
    out += text[i]
  }
  return out
}

/** Parse the readable style tags back into plain text and per-character effects (the inverse of toStyleTags). A
 *  "[" that is not a known tag is kept as literal text, so ordinary brackets survive. */
export function fromStyleTags(s: string): { text: string; fx: CharFx[] } {
  const apply = (cur: CharFx, tag: string): CharFx | null => {
    const t = tag.trim().toLowerCase()
    if (t in MOTION_ID) return { ...cur, m: MOTION_ID[t] }
    if (t === "fill:none" || t === "nofill") return { ...cur, f: null }
    if (t.startsWith("fill:")) { const v = parseInt(t.slice(5), 16); return isNaN(v) ? null : { ...cur, f: v & 0xffff } }
    if (t === "flash:none" || t === "noflash") return { ...cur, o: null }
    if (t.startsWith("flash:")) {
      const p = t.slice(6).split(",").map((x) => parseInt(x, 10))
      return p.length === 2 && p.every((x) => !isNaN(x)) ? { ...cur, o: [p[0], p[1]] } : null
    }
    return null
  }
  let text = "", fx: CharFx[] = [], cur: CharFx = NO_FX, i = 0
  while (i < s.length) {
    if (s[i] === "[") {
      const j = s.indexOf("]", i)
      if (j > i) {
        const next = apply(cur, s.slice(i + 1, j))
        if (next) { cur = next; i = j + 1; continue }
      }
    }
    const ch = String.fromCodePoint(s.codePointAt(i)!)
    for (let k = 0; k < ch.length; k++) { text += ch[k]; fx.push({ ...cur }) }   // one fx per UTF-16 unit
    i += ch.length
  }
  return { text, fx }
}

/** older saves kept only the motion per character (numbers) */
export function charFx(c: Composer): CharFx[] {
  return Array.from({ length: c.text.length }, (_, i) => {
    const v = (c.fx as unknown[] | undefined)?.[i]
    return typeof v === "number" ? { ...NO_FX, m: v } : ((v as CharFx | undefined) ?? NO_FX)
  })
}

/** The /api/preview spec (marquee_studio.render.render_message555). */
export function toSpec(c: Composer, palette: Record<string, number> | undefined) {
  const isColour = (v: string) => !!palette && v in palette
  const spec: Record<string, unknown> = {
    text: withEffects(c.text, charFx(c)),
    font: c.font,
    mode: c.motion === "scroll" ? "scroll" : "static",
    text_rate: c.textRate,
    flash_on_ms: c.motion === "flash" ? c.flashOnMs : 0,
    flash_off_ms: c.motion === "flash" ? c.flashOffMs : 0,
    outline: c.outline,
    outline_style: c.outlineStyle,
    value: c.value,
    amount: { mode: c.amountMode, decimals: c.amountDecimals, prefix: c.amountPrefix },
  }
  if (isColour(c.fill)) spec.colour = c.fill
  else spec.foreground = c.fill
  if (isColour(c.background)) spec.bg_colour = c.background
  else spec.background = c.background
  return spec
}

const KEY = "marquee-studio.composer.v1"

function load(): Composer {
  try {
    const s = localStorage.getItem(KEY)
    if (s) return { ...DEFAULT_COMPOSER, ...JSON.parse(s) }
  } catch {
    /* private window or blocked storage: start fresh */
  }
  return DEFAULT_COMPOSER
}

interface Ctx {
  c: Composer
  set: (patch: Partial<Composer>) => void
  reset: () => void
  undo: () => void
  redo: () => void
  canUndo: boolean
  canRedo: boolean
}

const HISTORY_MAX = 200
/** Edits that arrive as a burst (typing, a slider's drag) undo as one step; everything else is a step of its own. */
const BURST_MS = 600
const BURST_KEYS = new Set(["fx,text", "text", "textRate", "flashOnMs", "flashOffMs"])
const sameValue = (a: unknown, b: unknown) => Object.is(a, b) || JSON.stringify(a) === JSON.stringify(b)

const ComposerContext = createContext<Ctx | null>(null)

export function ComposerProvider({ children }: { children: ReactNode }) {
  const [c, setC] = useState<Composer>(load)
  useEffect(() => {
    try {
      localStorage.setItem(KEY, JSON.stringify(c))
    } catch {
      /* ignore */
    }
  }, [c])
  // the undo history: whole designs, this session only
  const current = useRef(c)
  const past = useRef<Composer[]>([])
  const future = useRef<Composer[]>([])
  const burst = useRef({ keys: "", at: 0 })
  const [can, setCan] = useState({ undo: false, redo: false })
  const show = useCallback((next: Composer) => {
    current.current = next
    setC(next)
    setCan({ undo: past.current.length > 0, redo: future.current.length > 0 })
  }, [])
  const set = useCallback((patch: Partial<Composer>) => {
    const prev = current.current
    const changed = (Object.keys(patch) as (keyof Composer)[]).filter((k) => !sameValue(prev[k], patch[k]))
    if (!changed.length) return
    const keys = changed.sort().join(","), now = Date.now()
    if (!(BURST_KEYS.has(keys) && keys === burst.current.keys && now - burst.current.at < BURST_MS)) {
      past.current.push(prev)
      if (past.current.length > HISTORY_MAX) past.current.shift()
    }
    burst.current = { keys, at: now }
    future.current = []
    show({ ...prev, ...patch })
  }, [show])
  const reset = useCallback(() => set(DEFAULT_COMPOSER), [set])
  const undo = useCallback(() => {
    const to = past.current.pop()
    if (!to) return
    future.current.push(current.current)
    burst.current = { keys: "", at: 0 }
    show(to)
  }, [show])
  const redo = useCallback(() => {
    const to = future.current.pop()
    if (!to) return
    past.current.push(current.current)
    burst.current = { keys: "", at: 0 }
    show(to)
  }, [show])
  const v = useMemo(() => ({ c, set, reset, undo, redo, canUndo: can.undo, canRedo: can.redo }), [c, set, reset, undo, redo, can])
  return <ComposerContext.Provider value={v}>{children}</ComposerContext.Provider>
}

export function useComposer() {
  const v = useContext(ComposerContext)
  if (!v) throw new Error("useComposer outside ComposerProvider")
  return v
}
