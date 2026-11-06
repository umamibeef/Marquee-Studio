import { useEffect, useMemo, useState, type ReactNode } from "react"
import { useQuery, useQueryClient } from "@tanstack/react-query"
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from "@marquee/ui/components/ui/dialog"
import { Button } from "@marquee/ui/components/ui/button"
import { Label } from "@marquee/ui/components/ui/label"
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@marquee/ui/components/ui/select"
import { Alert, AlertDescription } from "@marquee/ui/components/ui/alert"
import { Loader2, MonitorUp, TriangleAlert } from "@marquee/ui/lib/icons"
import { toast } from "@marquee/ui/lib/toast"
import { amountText } from "@marquee/sign-emu"
import { paintOf, type Placement } from "@/lib/apps"
import { api, postJson, useAssets, useConfig, type Assets, type Config } from "@/lib/api"
import { charFx, withEffects, type Composer } from "@/state/composer"

/** Compose's design as the Text app on an attract line (apps/text, its look from apps/include/show.h): what an
 *  attract line can carry of it, and what it cannot (the warnings). */
export function composeToPlacement(c: Composer, line: number, palette: Record<string, number>, signFonts: string[]): { p: Placement; warnings: string[] } {
  const warnings: string[] = []
  let text = withEffects(c.text, charFx(c))
  if (c.value !== "") {
    text = amountText(c.value, { mode: c.amountMode, decimals: c.amountDecimals, prefix: c.amountPrefix }) + " " + text
    warnings.push("The money value goes as plain text (no odometer).")
  }
  if (c.motion === "flash") warnings.push("An attract line does not flash: it is held instead.")
  const paint = (n: string, what: string, dflt: number) => {
    const v = paintOf(n, { palette } as Assets)
    if (v || n === "Off") return v
    warnings.push(`${what} ${n} is not one of the sign's effect animations: plain instead.`)
    return dflt
  }
  const font = signFonts.includes(c.font) ? c.font : ""
  if (!font) warnings.push(`${c.font} is not one of the sign's fonts: the firmware picks one.`)
  const styles = ["Off", "Thick", "Thin", "Drop Shadow"], colours = ["Black", "Red", "Green", "Yellow", "Blue", "Cyan", "Magenta", "White"]
  const p: Placement = {
    app: "text", line, enabled: true,
    settings: {
      messages: text, every: 10, font, colour: paint(c.fill, "The fill", 0x7e80), background: paint(c.background, "The background", 0),
      outline: c.outline === "Off" ? 0 : Math.max(1, styles.indexOf(c.outlineStyle)), outline_colour: Math.max(0, colours.indexOf(c.outline)),
      mode: c.motion === "scroll" ? 1 : 0, seconds: 50, speed: Math.max(40, c.textRate * 40),
    },
  }
  return { p, warnings }
}

const PORT_KEY = "marquee-studio.db9-port.v1"

/** "Show on the sign": the design as one of the sign's attract lines, uploaded with the other apps over the DB9
 *  (our firmware): it stays on the sign, and on the simulator (build/sim/db9) it is the simulator's preview. */
export function LineDialog({ composer, children }: { composer: Composer; children: ReactNode }) {
  const [open, setOpen] = useState(false)
  const { data: cfg } = useConfig()
  const { data: assets } = useAssets()
  const qc = useQueryClient()
  const ports = useQuery({ queryKey: ["ports"], queryFn: api.serialPorts, enabled: open, staleTime: 0 })
  const list = useMemo(() => ports.data?.ports ?? [], [ports.data])
  const [port, setPort] = useState(() => {
    try {
      return localStorage.getItem(PORT_KEY) ?? ""
    } catch {
      return ""
    }
  })
  useEffect(() => {                                       // the simulator's DB9 first, else a USB adapter
    if (list.length && !list.includes(port))
      setPort(list.find((p) => /build\/sim\/db9/.test(p)) ?? list.find((p) => /usb|serial/i.test(p)) ?? "")
  }, [list, port])
  const apps = useMemo(() => ((cfg as (Config & { apps?: Placement[] }) | undefined)?.apps ?? []).filter((a) => typeof a.app === "string"), [cfg])
  const [line, setLine] = useState("0")
  const { p: app, warnings } = useMemo(
    () => composeToPlacement(composer, +line, assets?.palette ?? {}, assets?.sign_fonts ?? []),
    [composer, line, assets],
  )
  const replaced = apps.find((a) => a.line === +line)
  const [busy, setBusy] = useState(false)

  const send = async () => {
    if (!cfg) return
    setBusy(true)
    try {
      const next = [...apps.filter((a) => a.line !== +line), app]
      await api.saveConfig({ ...cfg, apps: next } as Config)
      await qc.invalidateQueries({ queryKey: ["config"] })
      const r = await postJson<{ ok: boolean; listing?: string[]; error?: string }>("/api/upload_apps", { port, placements: next })
      if (!r.ok) throw new Error(r.error)
      try {
        localStorage.setItem(PORT_KEY, port)
      } catch {
        /* blocked storage: only the default is lost */
      }
      toast.success(`On attract line ${line}`, { description: (r.listing ?? []).join("\n") })
      setOpen(false)
    } catch (e) {
      toast.error("Upload failed", { description: String(e) })
    } finally {
      setBusy(false)
    }
  }

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>{children}</DialogTrigger>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>Show on the sign</DialogTitle>
          <DialogDescription>
            The design becomes one of the sign's attract lines (an app on the Apps page) and goes on with the other apps
            over the DB9. Our firmware then shows it by itself. Pick the simulator's DB9 to preview it there.
          </DialogDescription>
        </DialogHeader>
        <div className="grid gap-4">
          <div className="grid gap-2">
            <Label htmlFor="line-port">Serial port (DB9)</Label>
            <Select value={port} onValueChange={setPort}>
              <SelectTrigger id="line-port">
                <SelectValue placeholder={list.length ? "Choose the port" : "No serial ports found"} />
              </SelectTrigger>
              <SelectContent>
                {list.map((p) => (
                  <SelectItem key={p} value={p}>{ports.data?.labels?.[p] ?? p}</SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>
          <div className="grid gap-2">
            <Label htmlFor="line-line">Attract line</Label>
            <Select value={line} onValueChange={setLine}>
              <SelectTrigger id="line-line" className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {Array.from({ length: 16 }, (_, k) => {
                  const on = apps.find((a) => a.line === k)
                  return <SelectItem key={k} value={String(k)}>{`Line ${k}` + (on ? ` (now: ${on.app})` : "")}</SelectItem>
                })}
              </SelectContent>
            </Select>
          </div>
          {(warnings.length > 0 || replaced) && (
            <Alert>
              <TriangleAlert />
              <AlertDescription>
                {[...(replaced ? [`Replaces the ${replaced.app} app on line ${line}.`] : []), ...warnings].join(" ")}
              </AlertDescription>
            </Alert>
          )}
        </div>
        <DialogFooter>
          <Button variant="ghost" onClick={() => setOpen(false)}>
            Cancel
          </Button>
          <Button onClick={send} disabled={!port || busy || !cfg}>
            {busy ? <Loader2 className="animate-spin" /> : <MonitorUp />}
            Show on the sign
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
