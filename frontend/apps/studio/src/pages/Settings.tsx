import { useEffect, useState } from "react"
import { useQueryClient } from "@tanstack/react-query"
import { Button } from "@marquee/ui/components/ui/button"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@marquee/ui/components/ui/card"
import { Label } from "@marquee/ui/components/ui/label"
import { Slider } from "@marquee/ui/components/ui/slider"
import { Switch } from "@marquee/ui/components/ui/switch"
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@marquee/ui/components/ui/select"
import { Loader2 } from "@marquee/ui/lib/icons"
import { toast } from "@marquee/ui/lib/toast"
import { api, useAssets, useConfig, type Config } from "@/lib/api"
import { DEFAULT_LOOK, setLook, useLook } from "@/lib/hooks"

const WIDTHS = Array.from({ length: 16 }, (_, i) => (i + 1) * 20)
const HEIGHTS = [8, 16, 24, 32, 48, 64, 80]
const BAUDS = [9600, 19200, 38400, 57600, 115200]

/** The sign's flash rule (marquee_studio/flash.py): 25..6375 ms, and no more than 3 flashes a second. */
function Field({ label, children, htmlFor }: { label: string; children: React.ReactNode; htmlFor?: string }) {
  return (
    <div className="grid gap-2">
      <Label htmlFor={htmlFor} className="text-muted-foreground">
        {label}
      </Label>
      {children}
    </div>
  )
}

function PickNumber({ value, options, onChange, id }: { value: number; options: number[]; onChange: (v: number) => void; id?: string }) {
  const opts = options.includes(value) ? options : [...options, value].sort((a, b) => a - b)
  return (
    <Select value={String(value)} onValueChange={(v) => onChange(+v)}>
      <SelectTrigger id={id} className="w-full tabular-nums">
        <SelectValue />
      </SelectTrigger>
      <SelectContent>
        {opts.map((o) => (
          <SelectItem key={o} value={String(o)} className="tabular-nums">
            {o}
          </SelectItem>
        ))}
      </SelectContent>
    </Select>
  )
}

export function SettingsPage() {
  const { data: assets } = useAssets()
  const { data: saved } = useConfig()
  const qc = useQueryClient()
  const [cfg, setCfg] = useState<Config | null>(null)
  const [busy, setBusy] = useState(false)
  const look = useLook()
  useEffect(() => {
    if (!saved || cfg || !assets) return
    const c = structuredClone(saved)
    // name the display model when the saved size matches one (the config may predate the model field)
    if (!c.panel.model) {
      const m = assets.display_models.find((x) => x.w === c.panel.width && x.h === c.panel.height)
      if (m) c.panel.model = m.name
    }
    setCfg(c)
  }, [saved, cfg, assets])
  if (!cfg || !assets) return null

  const dirty =
    JSON.stringify({ ...cfg, panel: { ...cfg.panel, model: undefined } }) !==
    JSON.stringify({ ...saved, panel: { ...saved?.panel, model: undefined } })
  const models = assets.display_models
  const modelFor = (w: number, h: number, keep?: string) => {
    const k = models.find((m) => m.name === keep)
    if (k && k.w === w && k.h === h) return keep
    return models.find((m) => m.w === w && m.h === h)?.name ?? ""
  }
  const setPanel = (w: number, h: number, model?: string) =>
    setCfg({ ...cfg, panel: { width: w, height: h, model: modelFor(w, h, model ?? cfg.panel.model) } })
  const setProto = (k: keyof Config["PROTOCOL"], v: string | number) =>
    setCfg({ ...cfg, PROTOCOL: { ...cfg.PROTOCOL, [k]: v } })
  const protocols = assets.protocols.includes(cfg.PROTOCOL.Protocol)
    ? assets.protocols
    : [cfg.PROTOCOL.Protocol, ...assets.protocols]

  const save = async () => {
    setBusy(true)
    try {
      await api.saveConfig(cfg)
      await qc.invalidateQueries({ queryKey: ["config"] })
      await qc.invalidateQueries({ queryKey: ["preview"] })
      await qc.invalidateQueries({ queryKey: ["fx"] })
      await qc.invalidateQueries({ queryKey: ["font-sample"] })
      toast.success("Settings saved")
    } catch (e) {
      toast.error("Could not save", { description: (e as Error).message })
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="mx-auto w-full max-w-4xl space-y-6 px-4 py-6 pb-24 sm:px-6 lg:px-8">
      <Card>
        <CardHeader>
          <CardTitle>Panel</CardTitle>
          <CardDescription>The sign's size in LEDs. Previews render at this size.</CardDescription>
        </CardHeader>
        <CardContent className="grid gap-4 sm:grid-cols-3">
          <Field label="Display model" htmlFor="model">
            <Select
              value={cfg.panel.model || "custom"}
              onValueChange={(v) => {
                const m = models.find((x) => x.name === v)
                if (m) setPanel(m.w, m.h, m.name)
              }}
            >
              <SelectTrigger id="model" className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent className="max-h-80">
                <SelectItem value="custom" disabled>
                  Custom size
                </SelectItem>
                {models.map((m) => (
                  <SelectItem key={m.name} value={m.name}>
                    {m.name} <span className="text-muted-foreground tabular-nums">· {m.w} × {m.h}</span>
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </Field>
          <Field label="Width" htmlFor="pw">
            <PickNumber id="pw" value={cfg.panel.width} options={WIDTHS} onChange={(w) => setPanel(w, cfg.panel.height)} />
          </Field>
          <Field label="Height" htmlFor="ph">
            <PickNumber id="ph" value={cfg.panel.height} options={HEIGHTS} onChange={(h) => setPanel(cfg.panel.width, h)} />
          </Field>
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>Sign firmware</CardTitle>
          <CardDescription>What the sign runs. It decides how emoji are sent and previewed.</CardDescription>
        </CardHeader>
        <CardContent className="flex items-start justify-between gap-6">
          <div className="grid gap-1">
            <Label htmlFor="native-emoji">Native emoji</Label>
            <p className="text-sm text-muted-foreground">
              On for the from-source firmware with native emoji: emoji go to the sign as themselves and show with
              any font. Off for the stock firmware, which cannot draw emoji.
            </p>
          </div>
          <Switch
            id="native-emoji"
            checked={!!cfg.native_emoji}
            onCheckedChange={(v) => setCfg({ ...cfg, native_emoji: v })}
          />
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>Connection</CardTitle>
          <CardDescription>How the controller talks to the sign.</CardDescription>
        </CardHeader>
        <CardContent className="grid gap-4 sm:grid-cols-2">
          <Field label="Protocol" htmlFor="proto">
            <Select value={cfg.PROTOCOL.Protocol} onValueChange={(v) => setProto("Protocol", v)}>
              <SelectTrigger id="proto" className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {protocols.map((p) => (
                  <SelectItem key={p} value={p}>
                    {p}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </Field>
          <Field label="RS-485 baud" htmlFor="baud">
            <PickNumber id="baud" value={cfg.PROTOCOL["RS485 Baud"]} options={BAUDS} onChange={(v) => setProto("RS485 Baud", v)} />
          </Field>
          <Field label="Download mode" htmlFor="dmode">
            <Select value={cfg.PROTOCOL["Download Mode"]} onValueChange={(v) => setProto("Download Mode", v)}>
              <SelectTrigger id="dmode" className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="direct">Direct</SelectItem>
                <SelectItem value="modem">Modem</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="Download speed" htmlFor="drate">
            <PickNumber id="drate" value={cfg.PROTOCOL["Download Datarate"]} options={BAUDS} onChange={(v) => setProto("Download Datarate", v)} />
          </Field>
        </CardContent>
      </Card>

      <Card>
        <CardHeader className="flex flex-row items-start justify-between gap-4">
          <div className="grid gap-1.5">
            <CardTitle>Preview look</CardTitle>
            <CardDescription>How the LEDs are drawn on this screen. Saved in this browser.</CardDescription>
          </div>
          <Button variant="ghost" size="sm" onClick={() => setLook(DEFAULT_LOOK)}>
            Reset
          </Button>
        </CardHeader>
        <CardContent className="grid gap-6 sm:grid-cols-2">
          {(
            [
              ["Brightness", "brightness", 0.1, 1, 0.05, `${Math.round(look.brightness * 100)}%`],
              ["Gamma", "gamma", 1, 3, 0.1, look.gamma.toFixed(1)],
              ["Afterglow", "persist", 0, 0.9, 0.05, look.persist ? `${Math.round(look.persist * 100)}%` : "Off"],
            ] as const
          ).map(([label, k, min, max, step, shown]) => (
            <div key={k} className="grid gap-3">
              <div className="flex justify-between text-sm">
                <Label className="text-muted-foreground">{label}</Label>
                <span className="tabular-nums">{shown}</span>
              </div>
              <Slider min={min} max={max} step={step} value={[look[k]]} onValueChange={([v]) => setLook({ [k]: v })} aria-label={label} />
            </div>
          ))}
          <div className="flex items-center justify-between gap-3 self-end">
            <Label htmlFor="dots" className="text-muted-foreground">
              Round LED dots
            </Label>
            <Switch id="dots" checked={look.dots} onCheckedChange={(v) => setLook({ dots: v })} />
          </div>
        </CardContent>
      </Card>

      <div className="fixed right-6 bottom-6 z-30">
        <Button onClick={save} disabled={!dirty || busy} className="shadow-lg">
          {busy && <Loader2 className="animate-spin" />}
          {dirty ? "Save settings" : "Saved"}
        </Button>
      </div>
    </div>
  )
}
