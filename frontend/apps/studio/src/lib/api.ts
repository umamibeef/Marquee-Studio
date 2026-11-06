// The Flask studio API (studio/server.py). Every call goes through here.
import { useQuery, useQueryClient } from "@tanstack/react-query"
import type { Frames } from "@marquee/sign-player"

export interface AnimationInfo {
  w?: number
  h?: number
  depth?: number
  frames?: number
  speed_ms?: number
  sign_ms?: number
  author: string | null
  /** 0xC000 | n for effectNN.flc, else null */
  slot: number | null
  bytes: number
  error?: string
}

export interface FontInfo {
  origin: "sign" | "import"
  shareable: boolean
  credits: string[]
  cell?: [number, number]
  source?: string
  licence?: string
}

export interface DisplayModel {
  type: number
  word: number
  name: string
  w: number
  h: number
}

export interface Assets {
  animations: string[]
  fonts: string[]
  palette: Record<string, number>
  protocols: string[]
  font_info: Record<string, FontInfo>
  importable_fonts: { path: string; family: string; licence: string }[]
  effect_names: Record<string, string>
  animation_info: Record<string, AnimationInfo>
  special_effects: Record<string, number>
  outline_styles: string[]
  outline_colours: string[]
  amount_modes: Record<string, string>
  sign_fonts: string[]
  markup_entry: string[]
  markup_exit: string[]
  display_models: DisplayModel[]
}

export interface Level {
  name: string
  Address: number
  "Jackpot Level": number
  "Text Rate": number
  "Flash On": number
  "Flash Off": number
  [k: string]: unknown
}

export interface Config {
  panel: { width: number; height: number; model?: string }
  GENERAL?: Record<string, unknown>
  PROTOCOL: { Protocol: string; "Download Mode": string; "Download Datarate": number; "RS485 Baud": number }
  MODEL?: Record<string, unknown>
  levels: Level[]
  /** The sign runs firmware with native emoji: emoji are sent as UTF-8 and drawn from the sign's emoji sheet. */
  native_emoji?: boolean
}

export interface EmojiEntry {
  e: string
  cps: string
  name: string
  group: string
  file: string
  url: string
  /** In the sign's emoji sheet (firmware with native emoji). */
  native?: boolean
  /** Imported into the studio (a GIF or picture, with a private-use code point); it travels with the apps. */
  custom?: boolean
}

export class ApiError extends Error {}

export async function call<T>(url: string, init?: RequestInit): Promise<T> {
  const r = await fetch(url, init)
  let body: unknown = null
  try {
    body = await r.json()
  } catch {
    if (!r.ok) throw new ApiError(`${r.status} ${r.statusText}`)
  }
  const b = body as { ok?: boolean; error?: string }
  if (!r.ok || (b && b.ok === false)) throw new ApiError(b?.error || `${r.status} ${r.statusText}`)
  return body as T
}

export function postJson<T>(url: string, data: unknown): Promise<T> {
  return call<T>(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(data) })
}

export function postForm<T>(url: string, data: FormData): Promise<T> {
  return call<T>(url, { method: "POST", body: data })
}

export const api = {
  assets: () => call<Assets>("/api/assets"),
  config: () => call<Config>("/api/config"),
  saveConfig: (c: Config) => postJson<{ ok: boolean }>("/api/config", c),
  emoji: () => call<EmojiEntry[]>("/api/emoji"),
  importEmoji: (fd: FormData) => postForm<{ ok: boolean; e?: string; cps?: string; name?: string; error?: string }>("/api/emoji/import", fd),
  removeEmoji: (cps: string) => call<{ ok: boolean }>(`/api/emoji/custom/${cps}`, { method: "DELETE" }),
  serialPorts: () => call<{ ports: string[]; labels?: Record<string, string> }>("/api/serial_ports"),
  markup: (body: unknown) => postJson<MarkupResult>("/api/markup", body),
  sendRs485: (body: unknown) => postJson<{ ok: boolean; sent?: string; error?: string }>("/api/send_rs485", body),
  importFont: (path: string, upper: string) =>
    postJson<{ ok: boolean; name: string; cell: [number, number]; codes: number; licence: string }>(
      "/api/import_font",
      { path, upper },
    ),
  importAnimation: (fd: FormData) =>
    postForm<{ ok: boolean; name: string; frames_in: number; frames_out: number; speed_ms: number; bytes: number }>(
      "/api/import_animation",
      fd,
    ),
  uploadAsset: (fd: FormData) => postForm<{ ok: boolean; name: string }>("/api/upload_asset", fd),
}

export interface MarkupSegment {
  text?: string
  fg?: string | number
  bg?: string | number
  entry?: string
  entry_speed?: number
  hold?: number
  exit?: string | null
  exit_speed?: number | null
  font?: string | null
  centre?: boolean
  include?: number
}

export interface MarkupResult {
  ok: boolean
  error?: string
  bytes: string
  length: number
  segments: MarkupSegment[]
  warnings: string[]
  frame?: string
  frame_error?: string
  record?: { scrolls: boolean; width: number; font: string; window: [number, number] }
  preview?: Frames
  done?: boolean
  hang?: boolean
}

export function useAssets() {
  return useQuery({ queryKey: ["assets"], queryFn: api.assets, staleTime: Infinity })
}

export function useConfig() {
  return useQuery({ queryKey: ["config"], queryFn: api.config, staleTime: Infinity })
}

/** This sign's panel (studio/marquee_studio/config.py DEFAULT_CONFIG: ED0501, 200 x 16) until the saved settings
 *  say otherwise: every preview is drawn at this size. */
export const SIGN_PANEL = { width: 200, height: 16, model: "ED0501" }

/** The panel the studio previews at: the saved settings' panel, this sign's until they are loaded. */
export function usePanel() {
  const { data: cfg } = useConfig()
  const p = cfg?.panel ?? SIGN_PANEL
  return { W: p.width, H: p.height, model: p.model ?? SIGN_PANEL.model }
}

export function useEmoji() {
  return useQuery({ queryKey: ["emoji"], queryFn: api.emoji, staleTime: Infinity })
}

/** Refresh everything that depends on the content folder (after an import or upload). */
export function useRefreshContent() {
  const qc = useQueryClient()
  return () =>
    Promise.all([
      qc.invalidateQueries({ queryKey: ["assets"] }),
      qc.invalidateQueries({ queryKey: ["fx"] }),
      qc.invalidateQueries({ queryKey: ["font-sample"] }),
      qc.invalidateQueries({ queryKey: ["anim"] }),
      qc.invalidateQueries({ queryKey: ["preview"] }),
      qc.invalidateQueries({ queryKey: ["emoji"] }),
      qc.invalidateQueries({ queryKey: ["sign-files"] }),
    ])
}

export async function downloadBlob(blob: Blob, name: string) {
  const u = URL.createObjectURL(blob)
  const a = document.createElement("a")
  a.href = u
  a.download = name
  a.click()
  setTimeout(() => URL.revokeObjectURL(u), 1000)
}

export function rgb555ToCss(v: number) {
  const r = ((v >> 10) & 31) * 8.226
  const g = ((v >> 5) & 31) * 8.226
  const b = (v & 31) * 8.226
  return `rgb(${Math.round(r)}, ${Math.round(g)}, ${Math.round(b)})`
}
