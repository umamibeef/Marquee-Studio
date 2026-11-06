// Programmable apps in the Studio (docs/APPS.md): the catalogue (apps/*/app.json via /api/apps), placements (an app
// on an attract line with its settings, saved in the settings as "apps"), the bundles the server builds for them, the
// data feeds' preview values, and the paint codes the apps' settings use (apps/include/sign.h, show.h).
import { useQuery } from "@tanstack/react-query"
import { call, postJson, type Assets } from "./api"

export interface AppSetting { key: string; type: string; label: string; default?: string | number; choices?: string[] }
export interface AppManifest { title: string; description?: string; feed?: string; data?: string[]; settings?: AppSetting[] }
export interface Placement { app: string; line: number; enabled: boolean; settings: Record<string, string | number> }

export const useCatalogue = () => useQuery({ queryKey: ["apps"], queryFn: () => call<Record<string, AppManifest>>("/api/apps"), staleTime: Infinity })

/** an instance's bundle (built by the server), cached by its settings */
export function useBundle(p: Placement | null) {
  return useQuery({
    queryKey: ["app-bundle", p?.app, p?.settings],
    queryFn: async () => {
      const r = await fetch("/api/app_bundle", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ app: p!.app, settings: p!.settings }) })
      if (!r.ok) throw new Error((await r.json()).error ?? r.statusText)
      return new Uint8Array(await r.arrayBuffer())
    },
    enabled: !!p,
    staleTime: Infinity,
  })
}

/** a feed's current values for an app's settings (refreshed every 10 minutes) */
export function useFeed(feed: string | undefined, settings: Record<string, unknown>) {
  return useQuery({
    queryKey: ["feed", feed, settings],
    queryFn: async () => {
      const r = await postJson<{ ok: boolean; values?: Record<string, string>; error?: string }>(`/api/feed/${feed}`, settings)
      if (!r.ok) throw new Error(r.error)
      return r.values!
    },
    enabled: !!feed,
    staleTime: 600_000,
    refetchInterval: 600_000,
  })
}

/** the settings show.h reads, with their defaults */
export const DISPLAY_DEFAULTS = { font: "", colour: 0x7e80, background: 0, background_anim: "", outline: 0, outline_colour: 1, mode: 0, seconds: 50, speed: 255 }
export const WIPE = 0x8001

/** a paint code (sign.h) <-> the Studio's names: palette colours, the wipe, effect animations (effectNN.flc) */
export function paintOf(name: string, a: Assets | undefined): number {
  if (a?.palette[name] !== undefined) return a.palette[name]
  if (name === "Red/green/yellow wipe") return WIPE
  if (name.toLowerCase().startsWith("0x")) return parseInt(name, 16) & 0xffff   // a built-in effect word (stripes)
  const m = /^effect(\d\d)\.flc$/.exec(name)
  return m ? 0xc000 | +m[1] : 0
}
export function paintName(v: number, a: Assets | undefined): string {
  if (v === WIPE) return "Red/green/yellow wipe"
  if (v === 0x8002) return "0x8002"
  if ((v & 0xc000) === 0xc000) return `effect${String(v & 0x3fff).padStart(2, "0")}.flc`
  return Object.entries(a?.palette ?? {}).find(([, c]) => c === v)?.[0] ?? ""
}

/** sign seconds: local wall-clock seconds since 1970 */
export const signNow = (d = new Date()) => Math.floor(d.getTime() / 1000) - d.getTimezoneOffset() * 60
