import { useEffect, useState } from "react"

export function useDebounced<T>(value: T, ms = 250): T {
  const [v, setV] = useState(value)
  useEffect(() => {
    const t = setTimeout(() => setV(value), ms)
    return () => clearTimeout(t)
  }, [value, ms])
  return v
}

/** Display look of every LED preview (Settings), per viewer. */
export interface Look {
  brightness: number
  gamma: number
  persist: number
  dots: boolean
}
const LOOK_KEY = "marquee-studio.look.v1"
export const DEFAULT_LOOK: Look = { brightness: 1, gamma: 1, persist: 0.3, dots: true }
const listeners = new Set<(l: Look) => void>()
let current: Look = (() => {
  try {
    const s = localStorage.getItem(LOOK_KEY)
    if (s) return { ...DEFAULT_LOOK, ...JSON.parse(s) }
  } catch {
    /* ignore */
  }
  return DEFAULT_LOOK
})()

export function setLook(l: Partial<Look>) {
  current = { ...current, ...l }
  try {
    localStorage.setItem(LOOK_KEY, JSON.stringify(current))
  } catch {
    /* ignore */
  }
  listeners.forEach((f) => f(current))
}

export function useLook(): Look {
  const [l, setL] = useState(current)
  useEffect(() => {
    listeners.add(setL)
    return () => {
      listeners.delete(setL)
    }
  }, [])
  return l
}

/** Grapheme clusters that look like emoji (pictographic, flags, keycaps). */
export function emojiIn(text: string): string[] {
  const seg = new Intl.Segmenter(undefined, { granularity: "grapheme" })
  const out: string[] = []
  for (const { segment } of seg.segment(text)) {
    if (/\p{Extended_Pictographic}|\p{Regional_Indicator}|⃣/u.test(segment)) out.push(segment)
  }
  return out
}

export const stripVs = (e: string) => e.replace(/️/g, "")
