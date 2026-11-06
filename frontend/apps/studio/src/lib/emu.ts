// The studio's own sign: @marquee/sign-emu running in the browser (no server rendering). The server only hands over
// the sign's font sheets and effect animations as stored; everything the sign does with them is emulated here.

import { useQuery, useQueryClient, type QueryClient } from "@tanstack/react-query"
import { AttractPlayer, decodePng, Effects, FwFont, mqfFont, nativeEncode, nativeLayout, nativeParse, renderMessage, unpackEmojiSheet, type AttractLine, type EmojiSheet, type Img, type Spec } from "@marquee/sign-emu"
import type { Frames } from "@marquee/sign-player"
import { call } from "./api"

export interface SignFiles {
  fonts: FwFont[]
  /** the font files in the firmware's load order: fontNames[n] is the sign's font n */
  fontNames: string[]
  slots: Map<number, Uint8Array>
  /** the native emoji sheet, when the sign has native emoji */
  sheet?: EmojiSheet
  /** the fonts the firmware's automatic picks may choose (fonts.auto: firmware/auto_fonts.txt) */
  auto: ReadonlySet<string>
}

/** a font sheet or an animation from the content folder, as stored */
export const contentBytes = (kind: "fonts" | "animations", name: string) => bytes(`/api/content/${kind}/${name}`)

async function bytes(url: string): Promise<Uint8Array> {
  const r = await fetch(url)
  if (!r.ok) throw new Error(`${url}: ${r.status}`)
  return new Uint8Array(await r.arrayBuffer())
}

async function loadSign(): Promise<SignFiles> {
  const list = await call<{ fonts: string[]; effects: Record<string, string>; auto?: string[] }>("/api/sign_files")
  const fonts = await Promise.all(
    list.fonts.map(async (n) => {
      const b = await bytes(`/api/content/fonts/${n}`)
      return n.endsWith(".mqf") ? mqfFont(b, n) : new FwFont(await decodePng(b), n)
    }),
  )
  const slots = new Map<number, Uint8Array>()
  await Promise.all(
    Object.entries(list.effects).map(async ([n, name]) => slots.set(+n, await bytes(`/api/content/animations/${name}`))),
  )
  const r = await fetch("/api/emoji_sheet")
  const sheet = r.ok ? await unpackEmojiSheet(new Uint8Array(await r.arrayBuffer())) : undefined
  return { fonts, fontNames: list.fonts, slots, sheet, auto: new Set<string>(list.auto ?? []) }
}

/** The sign's fonts and effect animations, loaded once. */
export function useSign() {
  return useQuery({ queryKey: ["sign-files"], queryFn: loadSign, staleTime: Infinity })
}

const signFiles = (qc: QueryClient) => qc.ensureQueryData({ queryKey: ["sign-files"], queryFn: loadSign, staleTime: Infinity })

/** A message spec drawn here (renderMessage, the sign emulated in the browser): its frames for the LED player. An
 *  animation the spec names that is not an effect slot is fetched as stored and previewed in a free slot. */
export async function renderSpec(qc: QueryClient, spec: Spec, W: number, H: number, native = true): Promise<Frames> {
  return toFrames(await renderImgs(qc, spec, W, H, native), W, H)
}

export async function renderImgs(qc: QueryClient, spec: Spec, W: number, H: number, native = true): Promise<Img[]> {
  const sign = await signFiles(qc)
  const slotNames = new Set([...sign.slots.keys()].map((n) => `effect${String(n).padStart(2, "0")}.flc`))
  const anims = new Map<string, Uint8Array>()
  for (const n of [spec.foreground, spec.background])
    if (typeof n === "string" && n.endsWith(".flc") && !slotNames.has(n))
      anims.set(n, await qc.ensureQueryData({ queryKey: ["anim-bytes", n], queryFn: () => contentBytes("animations", n), staleTime: Infinity }))
  const nat = native && !!sign.sheet
  return renderMessage({ ...spec, native_emoji: nat, emoji_size: sign.sheet?.size ?? H }, W, H,
    { fonts: new Map(sign.fonts.map((f, i) => [sign.fontNames[i], f])), slots: sign.slots, anims, sheet: sign.sheet })
}

/** useQuery around renderSpec: null spec or !enabled renders nothing. */
export function useRendered(key: unknown[], spec: Spec | null, W: number, H: number, enabled = true) {
  const qc = useQueryClient()
  return useQuery({
    queryKey: ["emu", ...key, W, H],
    queryFn: () => renderSpec(qc, spec!, W, H),
    enabled: enabled && !!spec,
    staleTime: Infinity,
  })
}

/** A font's sample text on a panel just wide enough (and at least the font's cell high), as the sign draws it: the
 *  font picker's and the Fonts gallery's previews. */
export async function fontSample(qc: QueryClient, font: string, text: string, colour = "Amber"): Promise<Frames & { cell: [number, number] }> {
  const sign = await signFiles(qc)
  const k = sign.fontNames.indexOf(font)
  if (k < 0) throw new Error(`no font ${font}`)
  const ft = sign.fonts[k]
  const sheet = sign.sheet
  const tw = sheet ? nativeLayout(ft, nativeParse(nativeEncode(text, sheet), sheet), sheet.size)[1]
    : ft.textWidth(Uint8Array.from(Array.from(text, (c) => c.codePointAt(0)!).filter((v) => v < 256)))
  const W = Math.max(8, Math.min(400, tw + 4)), H = Math.max(8, Math.max(ft.ch, sheet?.size ?? 0) + 2)
  const frames = await renderImgs(qc, { text, font, mode: "static", colour } as Spec, W, H)
  return { ...toFrames(frames.slice(0, 1), W, H), cell: [ft.cw, ft.ch] }
}

/** RGB555 frames as the LED player takes them (base64, little-endian, 25 ms each). */
export function toFrames(frames: Img[], w: number, h: number): Frames {
  const all = new Uint8Array(frames.length * w * h * 2)
  frames.forEach((f, k) => all.set(new Uint8Array(f.d.buffer, f.d.byteOffset, f.d.byteLength), k * w * h * 2))
  let s = ""
  for (let i = 0; i < all.length; i += 0x8000) s += String.fromCharCode(...all.subarray(i, i + 0x8000))
  return { w, h, nframes: frames.length, ms: 25, data: btoa(s) }
}

/** One full turn of the attract loop, as the sign plays it: every enabled line once (and the black frame the
 *  firmware leaves between two lines), so the clip loops the way the sign goes round. Capped at maxFrames.
 *  starts: the frame each line begins at. */
export function attractRotation(sign: SignFiles, w: number, h: number, lines: (AttractLine | null)[],
                                maxFrames = 6000, fx = new Effects(sign.slots, 0)): { frames: Img[]; starts: { line: number; frame: number }[] } {
  const p = new AttractPlayer(sign.fonts, w, h, lines, fx, sign.sheet, sign.auto)
  const frames: Img[] = [p.frame()]                       // the first line starts here
  const first = p.line
  const starts = first >= 0 ? [{ line: first, frame: 0 }] : []
  if (first < 0) return { frames, starts }
  let tl = p.tl
  while (frames.length < maxFrames) {
    frames.push(p.frame())
    if (p.tl !== tl) {                                    // that was the line's last frame; the next one waits a frame
      tl = p.tl
      if (p.line === first) {
        frames.push(p.frame())                            // the black frame before the first line comes round again
        break
      }
      starts.push({ line: p.line, frame: frames.length + 1 })
    }
  }
  return { frames, starts }
}
