// How the message box marks per-character effects: each motion as its own underline, a fill as a tint behind the
// letters and an on/off time as an overline. The sign gets in-text effect commands (0x1F ..., state/composer.tsx
// withEffects); the controls are in FormatPalette.tsx.
import type { CSSProperties } from "react"
import { NO_FX, type CharFx } from "@/state/composer"

/** how the message box marks each motion (an underline of its own) */
export const MOTION_MARK: Record<number, CSSProperties> = {
  1: { textDecorationLine: "underline", textDecorationStyle: "wavy", textDecorationColor: "var(--chart-1)" },
  2: { textDecorationLine: "underline", textDecorationStyle: "dotted", textDecorationColor: "var(--chart-2)" },
  3: { textDecorationLine: "underline", textDecorationStyle: "dashed", textDecorationColor: "var(--chart-3)" },
  4: { textDecorationLine: "underline", textDecorationStyle: "double", textDecorationColor: "var(--chart-4)" },
}

/** an effect word's look in the message box: an RGB555 colour as itself, an effect as a neutral tint */
function fillCss(w: number): string {
  if (w < 0x8000) {
    const r = (w >> 10) & 31, g = (w >> 5) & 31, b = w & 31
    return `rgb(${r * 8} ${g * 8} ${b * 8} / 0.35)`
  }
  return "color-mix(in oklab, var(--chart-5) 35%, transparent)"
}

/** The marks, drawn behind the message box with the same text and layout (the box itself stays plain). */
export function MotionBackdrop({ text, fx, className, held }: {
  text: string
  fx: CharFx[]
  className: string
  /** a selection to keep showing while the box has lost the focus to a palette menu */
  held?: [number, number] | null
}) {
  const inHeld = (i: number) => !!held && i >= held[0] && i < held[1]
  const runs: { e: CharFx; s: string; held: boolean }[] = []
  const same = (p: CharFx, q: CharFx) => p.m === q.m && p.f === q.f && (p.o?.join() ?? "") === (q.o?.join() ?? "")
  for (let i = 0; i < text.length; i++) {
    const e = fx[i] ?? NO_FX
    const last = runs[runs.length - 1]
    if (last && same(last.e, e) && last.held === inHeld(i)) last.s += text[i]
    else runs.push({ e, s: text[i], held: inHeld(i) })
  }
  return (
    <div aria-hidden className={className}>
      {runs.map((r, i) => {
        const st: CSSProperties = { ...(r.e.m ? MOTION_MARK[r.e.m] : {}), textDecorationThickness: "2px", textUnderlineOffset: "4px" }
        if (r.e.o) {
          st.textDecorationLine = [r.e.m ? "underline" : "", "overline"].join(" ").trim()
          if (!r.e.m) st.textDecorationColor = "var(--muted-foreground)"
        }
        if (r.e.f !== null) {
          st.backgroundColor = fillCss(r.e.f)
          st.borderRadius = "3px"
        }
        if (r.held) st.boxShadow = "inset 0 0 0 100px color-mix(in oklab, var(--primary) 25%, transparent)"
        return <span key={i} data-held={r.held || undefined} style={r.held || r.e.m || r.e.f !== null || r.e.o ? st : undefined}>{r.s}</span>
      })}
      {"​"}
    </div>
  )
}
