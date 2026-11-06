// The Apps page: programmable apps (docs/APPS.md) on the sign's attract lines. Each placement is an app from apps/ on
// a line, with its settings; the preview runs the apps' own bytecode in the browser (sign-emu appvm.ts, the same as
// the firmware), the upload puts them in the sign's content store over the DB9.
import { useEffect, useMemo, useState } from "react"
import { useQueries, useQuery, useQueryClient } from "@tanstack/react-query"
import { Button } from "@marquee/ui/components/ui/button"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@marquee/ui/components/ui/card"
import { Input } from "@marquee/ui/components/ui/input"
import { Switch } from "@marquee/ui/components/ui/switch"
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@marquee/ui/components/ui/select"
import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { Alert, AlertDescription } from "@marquee/ui/components/ui/alert"
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@marquee/ui/components/ui/tabs"
import { Badge } from "@marquee/ui/components/ui/badge"
import { Plus, Trash2, TriangleAlert, Upload } from "@marquee/ui/lib/icons"
import { toast } from "@marquee/ui/lib/toast"
import { AppInstance, Effects, parseBundle, type AttractLine } from "@marquee/sign-emu"
import { api, postJson, useAssets, useConfig, usePanel, type Config } from "@/lib/api"
import { attractRotation, toFrames, useSign } from "@/lib/emu"
import { DISPLAY_DEFAULTS, paintName, paintOf, signNow, useCatalogue, useFeed, type AppManifest, type AppSetting, type Placement } from "@/lib/apps"
import { Stage } from "@/components/Stage"
import { ColourPicker } from "@/components/ColourPicker"
import { AnimationPicker } from "@/components/AnimationPicker"
import { OutlinePicker } from "@/components/OutlinePicker"
import { FontPicker } from "@/components/FontPicker"
import { Section } from "@/components/Section"
import { EmojiInput, Field, FormatCodes } from "@/components/AppFields"

const OUTLINE_STYLES = ["None", "Thick", "Thin", "Drop Shadow"]
const OUTLINE_COLOURS = ["Black", "Red", "Green", "Yellow", "Blue", "Cyan", "Magenta", "White"]

const str = (v: unknown) => (v === undefined || v === null ? "" : String(v))
const num = (v: unknown, d = 0) => (Number.isFinite(Number(v)) && v !== "" ? Number(v) : d)

/** "YYYY-MM-DDTHH:MM" <-> sign seconds (wall clock, no time zone) */
const toLocalInput = (s: number) => (s ? new Date(s * 1000).toISOString().slice(0, 16) : "")
const fromLocalInput = (v: string) => (v ? Math.floor(Date.parse(v + ":00Z") / 1000) : 0)

/** one of an app's own settings, by its type in app.json */
function SettingField({ s, value, set, data }: { s: AppSetting; value: unknown; set: (v: string | number) => void; data?: string[] }) {
  switch (s.type) {
    case "strftime":
      return (
        <div className="flex flex-wrap items-end gap-3">
          <Field label={s.label}><EmojiInput className="w-64" value={str(value)} onChange={set} /></Field>
          <FormatCodes format={str(value)} onPick={set} />
        </div>
      )
    case "template":
      return (
        <div className="grid gap-2">
          <Field label={s.label}><EmojiInput className="w-80" value={str(value)} onChange={set} /></Field>
          <div className="flex flex-wrap items-center gap-1.5 text-sm text-muted-foreground">
            Fields:
            {(data ?? []).map((k) => (
              <Badge key={k} variant="outline" className="cursor-pointer font-mono" onClick={() => set(str(value) + `{${k}}`)}>{`{${k}}`}</Badge>
            ))}
          </div>
        </div>
      )
    case "lines":
      return <Field label={s.label} className="min-w-72 flex-1"><EmojiInput multiline value={str(value)} onChange={set} /></Field>
    case "int":
      return (
        <Field label={s.label}>
          <Input className="w-32 tabular-nums" type="number" value={num(value)} onChange={(e) => set(Math.trunc(+e.target.value))} />
        </Field>
      )
    case "datetime":
      return (
        <Field label={s.label}>
          <Input className="w-56" type="datetime-local" value={toLocalInput(num(value))} onChange={(e) => set(fromLocalInput(e.target.value))} />
        </Field>
      )
    case "since":
      return (
        <Field label={s.label}>
          <div className="flex items-center gap-2">
            <Input className="w-56" type="datetime-local" value={toLocalInput(num(value))} onChange={(e) => set(fromLocalInput(e.target.value))} />
            <Button variant="outline" onClick={() => set(signNow())}>Now</Button>
          </div>
        </Field>
      )
    case "choice":
      return (
        <Field label={s.label}>
          <Select value={str(value)} onValueChange={set}>
            <SelectTrigger className="w-44"><SelectValue /></SelectTrigger>
            <SelectContent>{(s.choices ?? []).map((c) => <SelectItem key={c} value={c}>{c}</SelectItem>)}</SelectContent>
          </Select>
        </Field>
      )
    default:
      return <Field label={s.label}><EmojiInput className="w-64" value={str(value)} onChange={set} /></Field>
  }
}

/** the look every app takes (apps/include/show.h): letters, background, outline, font, how it is shown */
function Look({ p, set }: { p: Placement; set: (v: Record<string, string | number>) => void }) {
  const { data: assets } = useAssets()
  const v = { ...DISPLAY_DEFAULTS, ...p.settings }
  const slotAnims = (assets?.animations ?? []).filter((n) => /^effect\d\d\.flc$/.test(n))
  const font = str(v.font)
  return (
    <Tabs defaultValue="letters" className="gap-4">
      <TabsList>
        <TabsTrigger value="letters">Letters</TabsTrigger>
        <TabsTrigger value="background">Background</TabsTrigger>
        <TabsTrigger value="font">Font</TabsTrigger>
        <TabsTrigger value="display">Display</TabsTrigger>
      </TabsList>
      <TabsContent value="letters" className="space-y-6">
        <Section title="Colour">
          <ColourPicker label="Letter colour" value={paintName(num(v.colour), assets)} onChange={(n) => set({ colour: paintOf(n, assets) })}
            colours={Object.keys(assets?.palette ?? {}).filter((n) => n !== "Off")} />
        </Section>
        <Section title="Special">
          <ToggleGroup type="single" variant="outline" value={num(v.colour) === 0x8001 ? "wipe" : ""} onValueChange={(x) => x && set({ colour: 0x8001 })}>
            <ToggleGroupItem value="wipe" className="px-3">Red/green/yellow wipe</ToggleGroupItem>
          </ToggleGroup>
        </Section>
        <Section title="Animated fill" hint="The sign's effect animations. Hover to play.">
          <AnimationPicker role="fg" value={paintName(num(v.colour), assets)} onChange={(n) => set({ colour: paintOf(n, assets) })} font={font || "fonte04.png"} only={slotAnims} />
        </Section>
        <Section title="Outline">
          <OutlinePicker value={{ outline: num(v.outline) ? OUTLINE_COLOURS[num(v.outline_colour) & 7] : "Off", outlineStyle: OUTLINE_STYLES[num(v.outline)] ?? "Thick" }}
            onChange={(o) => set(o.outline === "Off" ? { outline: 0 } : { outline: Math.max(1, OUTLINE_STYLES.indexOf(o.outlineStyle)), outline_colour: Math.max(0, OUTLINE_COLOURS.indexOf(o.outline)) })} />
        </Section>
      </TabsContent>
      <TabsContent value="background" className="space-y-6">
        <Section title="Colour">
          <ColourPicker label="Background colour" value={paintName(num(v.background), assets)} onChange={(n) => set({ background: paintOf(n, assets) })} />
        </Section>
        <Section title="Effect fill" hint="The sign's effect animations as the background fill. Hover to play.">
          <AnimationPicker role="bg" value={paintName(num(v.background), assets)} onChange={(n) => set({ background: paintOf(n, assets) })} font={font || "fonte04.png"} only={slotAnims} />
        </Section>
        <Section title="Animation behind the text" hint="Any animation in the library (imported GIFs too): bundled with the app and played behind its text, tiled across the panel.">
          <div className="grid gap-3">
            <ToggleGroup type="single" variant="outline" value={str(v.background_anim) ? "anim" : "none"} onValueChange={(x) => x === "none" && set({ background_anim: "" })}>
              <ToggleGroupItem value="none" className="px-3">None</ToggleGroupItem>
              <ToggleGroupItem value="anim" className="px-3" disabled={!str(v.background_anim)}>{str(v.background_anim) || "Pick one below"}</ToggleGroupItem>
            </ToggleGroup>
            <AnimationPicker role="bg" value={str(v.background_anim)} onChange={(n) => set({ background_anim: n })} font={font || "fonte04.png"} />
          </div>
        </Section>
      </TabsContent>
      <TabsContent value="font" className="space-y-4">
        <ToggleGroup type="single" variant="outline" value={font ? "own" : "auto"} onValueChange={(x) => x && set({ font: x === "auto" ? "" : font || "fonte04.png" })}>
          <ToggleGroupItem value="auto" className="px-3">Automatic: the largest that fits</ToggleGroupItem>
          <ToggleGroupItem value="own" className="px-3">This font</ToggleGroupItem>
        </ToggleGroup>
        {font && <FontPicker value={font} onChange={(f) => set({ font: f })} sample="12:34" />}
      </TabsContent>
      <TabsContent value="display">
        <div className="flex flex-wrap items-end gap-4">
          <Field label="Shown">
            <ToggleGroup type="single" variant="outline" value={num(v.mode) ? "scroll" : "hold"} onValueChange={(x) => x && set({ mode: x === "scroll" ? 1 : 0 })}>
              <ToggleGroupItem value="hold">Held</ToggleGroupItem>
              <ToggleGroupItem value="scroll">Scrolling</ToggleGroupItem>
            </ToggleGroup>
          </Field>
          {!num(v.mode) && (
            <Field label="Held for (s)">
              <Input className="w-24 tabular-nums" type="number" min={0.1} max={60} step={0.1} value={num(v.seconds, 50) / 10}
                onChange={(e) => set({ seconds: Math.round(Math.min(60, Math.max(0.1, +e.target.value)) * 10) })} />
            </Field>
          )}
          <Field label="Speed (px/s)">
            <Input className="w-24 tabular-nums" type="number" min={10} max={400} value={num(v.speed, 255)} onChange={(e) => set({ speed: Math.round(Math.min(400, Math.max(10, +e.target.value))) })} />
          </Field>
        </div>
      </TabsContent>
    </Tabs>
  )
}

function defaults(m: AppManifest | undefined): Record<string, string | number> {
  const out: Record<string, string | number> = {}
  for (const s of m?.settings ?? []) out[s.key] = s.type === "since" ? signNow() : (s.default ?? "")
  return out
}

export function AppsPage() {
  const { data: saved } = useConfig()
  const { data: catalogue } = useCatalogue()
  const sign = useSign()
  const qc = useQueryClient()
  const { W, H } = usePanel()
  const [list, setList] = useState<Placement[] | null>(null)
  const [newApp, setNewApp] = useState("clock")
  useEffect(() => {                                       // placements of the programmable apps (older entries dropped)
    if (saved && list === null)
      setList((((saved as Config & { apps?: Placement[] }).apps ?? []) as Placement[]).filter((p) => typeof p.app === "string"))
  }, [saved, list])
  const placements = list ?? []
  const [tick, setTick] = useState(0)
  useEffect(() => {
    const t = setInterval(() => setTick((k) => k + 1), 60_000)
    return () => clearInterval(t)
  }, [])

  // the bundles (built by the server from apps/<name>) and the feeds' values for the preview
  const bundles = useQueries({
    queries: placements.map((p) => ({
      queryKey: ["app-bundle", p.app, p.settings],
      queryFn: async () => {
        const r = await fetch("/api/app_bundle", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ app: p.app, settings: p.settings }) })
        if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error ?? r.statusText)
        return parseBundle(new Uint8Array(await r.arrayBuffer()))
      },
      staleTime: Infinity,
    })),
  })
  const weather = placements.find((p) => catalogue?.[p.app]?.feed === "weather")
  const feed = useFeed(weather ? "weather" : undefined, weather ? { ...defaults(catalogue?.[weather.app]), ...weather.settings } : {})

  const turn = useMemo(() => {
    if (!sign.data) return null
    const fx = new Effects(sign.data.slots, 0)
    const now = signNow()
    const lines: (AttractLine | null)[] = Array(16).fill(null)
    const errors: string[] = []
    placements.forEach((p, k) => {
      const b = bundles[k]?.data
      if (!p.enabled || !b || lines[p.line]) return
      const app = new AppInstance(b, { fonts: sign.data!.fonts, fontNames: sign.data!.fontNames, sheet: sign.data!.sheet, fx, now: () => now, auto: sign.data!.auto }, W, H)
      if (catalogue?.[p.app]?.feed && feed.data)
        for (const [key, v] of Object.entries(feed.data)) app.data.set(key, { value: new TextEncoder().encode(v), at: now })
      if (!app.init()) errors.push(`line ${p.line} (${p.app}): ${app.error}`)
      lines[p.line] = { data: new Uint8Array([0x20]), effect: 0x80008000, program: app }
    })
    const r = attractRotation(sign.data, W, H, lines, 6000, fx)
    return { frames: toFrames(r.frames, W, H), starts: r.starts, errors }
  }, [sign.data, bundles.map((b) => b.dataUpdatedAt).join(), feed.data, tick, W, H])  // eslint-disable-line react-hooks/exhaustive-deps

  const update = (i: number, v: Partial<Placement>) => setList(placements.map((p, k) => (k === i ? { ...p, ...v } : p)))
  const add = () => {
    const used = new Set(placements.map((p) => p.line))
    const line = Array.from({ length: 16 }, (_, k) => k).find((k) => !used.has(k)) ?? 0
    setList([...placements, { app: newApp, line, enabled: true, settings: defaults(catalogue?.[newApp]) }])
  }
  const dirty = saved && JSON.stringify((saved as Config & { apps?: Placement[] }).apps ?? []) !== JSON.stringify(placements)
  const save = async () => {
    if (!saved) return
    await api.saveConfig({ ...saved, apps: placements } as Config)
    await qc.invalidateQueries({ queryKey: ["config"] })
    toast.success("Apps saved")
  }
  const ports = useQuery({ queryKey: ["ports"], queryFn: api.serialPorts, staleTime: 0 })
  const [port, setPort] = useState("")
  const [uploading, setUploading] = useState(false)
  useEffect(() => {
    const l = ports.data?.ports ?? []
    if (l.length && !l.includes(port)) setPort(l.find((p) => /build\/sim\/db9|usb|serial/i.test(p)) ?? l[0])
  }, [ports.data, port])
  const upload = async () => {
    setUploading(true)
    try {
      const r = await postJson<{ ok: boolean; listing?: string[]; apps?: number; bytes?: number; fed?: string[]; error?: string }>("/api/upload_apps", { port, placements })
      if (!r.ok) throw new Error(r.error)
      toast.success(`${r.apps} app${r.apps === 1 ? "" : "s"} on the sign (${r.bytes} bytes)`, { description: (r.listing ?? []).join("\n") })
    } catch (e) {
      toast.error("Upload failed", { description: String(e) })
    } finally {
      setUploading(false)
    }
  }
  const seconds = (f: number) => ((f * 25) / 1000).toFixed(1)

  return (
    <div className="grid gap-6">
      <Stage
        frames={turn?.frames}
        busy={sign.isLoading || bundles.some((b) => b.isFetching)}
        footer={
          <span className="text-sm text-muted-foreground">
            {turn?.errors.length
              ? turn.errors.join("; ")
              : turn && turn.starts.length
                ? "One turn of the sign's attract loop, the apps' own code running here as on the sign: " +
                  turn.starts.map((s) => `line ${s.line} at ${seconds(s.frame)} s`).join(", ") + `; ${seconds(turn.frames.nframes)} s round.`
                : sign.isError
                  ? `The sign's fonts did not load: ${String(sign.error)}`
                  : "No app is on: the sign shows its attract animations."}
          </span>
        }
      />
      <Card>
        <CardHeader>
          <CardTitle>Apps</CardTitle>
          <CardDescription>
            Each app is a small program that draws its own line of the sign's attract loop. The sign runs them by itself
            once they are uploaded; the weather app gets its weather from this Studio while it is connected.
          </CardDescription>
        </CardHeader>
        <CardContent className="grid gap-6">
          {placements.map((p, i) => {
            const m = catalogue?.[p.app]
            const err = bundles[i]?.error
            return (
              <div key={i} className="grid gap-4 rounded-lg border p-4">
                <div className="flex flex-wrap items-end gap-4">
                  <Field label="App">
                    <Select value={p.app} onValueChange={(a) => update(i, { app: a, settings: defaults(catalogue?.[a]) })}>
                      <SelectTrigger className="w-44"><SelectValue /></SelectTrigger>
                      <SelectContent>{Object.entries(catalogue ?? {}).map(([k, v]) => <SelectItem key={k} value={k}>{v.title}</SelectItem>)}</SelectContent>
                    </Select>
                  </Field>
                  <Field label="Line">
                    <Select value={String(p.line)} onValueChange={(v) => update(i, { line: +v })}>
                      <SelectTrigger className="w-20 tabular-nums"><SelectValue /></SelectTrigger>
                      <SelectContent>{Array.from({ length: 16 }, (_, k) => <SelectItem key={k} value={String(k)}>{String(k)}</SelectItem>)}</SelectContent>
                    </Select>
                  </Field>
                  <div className="flex items-center gap-2 pb-2">
                    <Switch checked={p.enabled} onCheckedChange={(v) => update(i, { enabled: v })} aria-label="On" />
                    <span className="text-sm text-muted-foreground">{p.enabled ? "On" : "Off"}</span>
                  </div>
                  <Button variant="ghost" className="ml-auto" onClick={() => setList(placements.filter((_, k) => k !== i))}><Trash2 /> Remove</Button>
                </div>
                {m?.description && <p className="text-sm text-muted-foreground">{m.description}</p>}
                <div className="flex flex-wrap items-start gap-4">
                  {(m?.settings ?? []).map((s) => (
                    <SettingField key={s.key} s={s} value={p.settings[s.key] ?? s.default} data={m?.data}
                      set={(v) => update(i, { settings: { ...p.settings, [s.key]: v } })} />
                  ))}
                </div>
                {m?.feed && (
                  <p className="text-sm text-muted-foreground">
                    {feed.data ? "Now: " + Object.entries(feed.data).map(([k, v]) => `${k} ${v}`).join(", ") : feed.isError ? `The ${m.feed} feed failed: ${String(feed.error)}` : "Fetching…"}
                  </p>
                )}
                <Look p={p} set={(v) => update(i, { settings: { ...p.settings, ...v } })} />
                {err && (
                  <Alert><TriangleAlert /><AlertDescription>{String(err)}</AlertDescription></Alert>
                )}
              </div>
            )
          })}
          <div className="flex flex-wrap items-center gap-2">
            <Select value={newApp} onValueChange={setNewApp}>
              <SelectTrigger className="w-44"><SelectValue /></SelectTrigger>
              <SelectContent>{Object.entries(catalogue ?? {}).map(([k, v]) => <SelectItem key={k} value={k}>{v.title}</SelectItem>)}</SelectContent>
            </Select>
            <Button variant="outline" onClick={add}><Plus /> Add app</Button>
            <Button className="ml-auto" variant={dirty ? "default" : "outline"} disabled={!dirty} onClick={save}>Save</Button>
          </div>
          <div className="flex flex-wrap items-center gap-2 border-t pt-4">
            <span className="text-sm text-muted-foreground">Upload to</span>
            <Select value={port} onValueChange={setPort}>
              <SelectTrigger className="w-80"><SelectValue placeholder="No serial port" /></SelectTrigger>
              <SelectContent>{(ports.data?.ports ?? []).map((p) => <SelectItem key={p} value={p}>{ports.data?.labels?.[p] ?? p}</SelectItem>)}</SelectContent>
            </Select>
            <Button disabled={!port || uploading || !placements.length} onClick={upload}><Upload /> {uploading ? "Uploading..." : "Upload to the sign"}</Button>
            <span className="text-sm text-muted-foreground">Over the DB9 into the sign's content store (our firmware); it keeps them through power cycles.</span>
          </div>
        </CardContent>
      </Card>
    </div>
  )
}
