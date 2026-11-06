import { useEffect, useMemo, useState } from "react"
import { keepPreviousData, useQuery } from "@tanstack/react-query"
import { Button } from "@marquee/ui/components/ui/button"
import { Card, CardContent } from "@marquee/ui/components/ui/card"
import { Input } from "@marquee/ui/components/ui/input"
import { Label } from "@marquee/ui/components/ui/label"
import { Slider } from "@marquee/ui/components/ui/slider"
import { Switch } from "@marquee/ui/components/ui/switch"
import { Textarea } from "@marquee/ui/components/ui/textarea"
import { Badge } from "@marquee/ui/components/ui/badge"
import { Alert, AlertDescription } from "@marquee/ui/components/ui/alert"
import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@marquee/ui/components/ui/select"
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "@marquee/ui/components/ui/collapsible"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import {
  ArrowDown,
  ArrowDownRight,
  ArrowLeft,
  ArrowRight,
  ArrowUp,
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  CircleCheck,
  Plus,
  Repeat,
  Send,
  Trash2,
  TriangleAlert,
  type LucideIcon,
} from "@marquee/ui/lib/icons"
import { cn } from "@marquee/ui/lib/utils"
import { api, rgb555ToCss, useAssets, useConfig, type MarkupSegment, usePanel } from "@/lib/api"
import { useDebounced } from "@/lib/hooks"
import { Stage } from "@/components/Stage"
import { textlineFrames } from "@marquee/sign-emu"
import { toFrames, useSign } from "@/lib/emu"
import { SendDialog } from "@/components/SendDialog"

type Seg = MarkupSegment & { id: number }

let nextId = 1
const withId = (s: MarkupSegment): Seg => ({ ...s, id: nextId++ })

const DEFAULT: MarkupSegment[] = [
  { text: "UMAMI MARQUEE", fg: "Green", bg: "Off", entry: "left", entry_speed: 80, hold: 10, exit: "left", exit_speed: 80, font: null, centre: true },
  { text: "HELLO", fg: "Red", bg: "Off", entry: "right", entry_speed: 80, hold: 10, exit: "up", exit_speed: 40, font: null, centre: true },
]

// "Entry from" is where the text comes from; the arrow shows the way it moves.
const ENTRY_ICON: Record<string, LucideIcon> = {
  right: ArrowLeft,
  left: ArrowRight,
  bottom: ArrowUp,
  top: ArrowDown,
  "top-left": ArrowDownRight,
}
const EXIT_ICON: Record<string, LucideIcon> = { left: ArrowLeft, right: ArrowRight, up: ArrowUp, down: ArrowDown }

const KEY = "marquee-studio.lines.v1"

function load(): Seg[] {
  try {
    const s = localStorage.getItem(KEY)
    if (s) return (JSON.parse(s) as MarkupSegment[]).map(withId)
  } catch {
    /* ignore */
  }
  return DEFAULT.map(withId)
}

/** Rough seconds of one segment (entry + hold + exit), for the timeline's proportions. */
function seconds(s: MarkupSegment, W: number) {
  if (s.include !== undefined) return 1.5
  const entry = W / Math.max(1, s.entry_speed ?? 40)
  const hold = (s.hold ?? 0) * 0.1
  const exit = s.hold ? W / Math.max(1, s.exit_speed ?? 40) : W / Math.max(1, s.entry_speed ?? 40)
  return entry + hold + exit
}

function colourName(v: string | number | undefined, pal: Record<string, number>) {
  if (typeof v === "string") return v
  if (typeof v === "number") return Object.keys(pal).find((k) => pal[k] === v) ?? `0x${v.toString(16).padStart(4, "0")}`
  return "Off"
}

function ColourSelect({ value, onChange, label }: { value: string; onChange: (v: string) => void; label: string }) {
  const { data: assets } = useAssets()
  const pal = assets?.palette ?? {}
  const names = value in pal ? Object.keys(pal) : [value, ...Object.keys(pal)]
  return (
    <Select value={value} onValueChange={onChange}>
      <SelectTrigger className="w-full" aria-label={label}>
        <SelectValue />
      </SelectTrigger>
      <SelectContent>
        {names.map((n) => (
          <SelectItem key={n} value={n}>
            <span className="size-3 rounded-full ring-1 ring-white/15" style={{ background: n in pal ? rgb555ToCss(pal[n]) : "#888" }} />
            {n}
          </SelectItem>
        ))}
      </SelectContent>
    </Select>
  )
}

function SliderRow({ label, value, unit, min, max, step = 1, onChange, disabled }: {
  label: string; value: number; unit: string; min: number; max: number; step?: number; onChange: (v: number) => void; disabled?: boolean
}) {
  return (
    <div className={cn("grid gap-2.5", disabled && "opacity-50")}>
      <div className="flex justify-between text-sm">
        <Label className="text-muted-foreground">{label}</Label>
        <span className="tabular-nums">{unit}</span>
      </div>
      <Slider min={min} max={max} step={step} value={[value]} onValueChange={([v]) => onChange(v)} disabled={disabled} aria-label={label} />
    </div>
  )
}

function SegmentEditor({ s, onChange }: { s: Seg; onChange: (patch: Partial<MarkupSegment>) => void }) {
  const { data: assets } = useAssets()
  const pal = assets?.palette ?? {}
  if (s.include !== undefined) {
    return (
      <div className="grid max-w-sm gap-3">
        <SliderRow label="Show message line" value={s.include} unit={`line ${s.include}`} min={0} max={16} onChange={(v) => onChange({ include: v })} />
        <p className="text-sm text-muted-foreground">Plays that line here, then carries on.</p>
      </div>
    )
  }
  const hold = s.hold ?? 0
  return (
    <div className="grid gap-6 lg:grid-cols-2">
      <div className="grid content-start gap-5">
        <div className="grid gap-2">
          <Label htmlFor="seg-text">Text</Label>
          <Input id="seg-text" value={s.text ?? ""} onChange={(e) => onChange({ text: e.target.value })} className="h-10 text-base" />
        </div>
        <div className="grid grid-cols-2 gap-4">
          <div className="grid gap-2">
            <Label>Text colour</Label>
            <ColourSelect label="Text colour" value={colourName(s.fg, pal)} onChange={(v) => onChange({ fg: v })} />
          </div>
          <div className="grid gap-2">
            <Label>Behind the letters</Label>
            <ColourSelect label="Background" value={colourName(s.bg, pal)} onChange={(v) => onChange({ bg: v })} />
          </div>
        </div>
        <div className="grid grid-cols-2 items-end gap-4">
          <div className="flex h-9 items-center gap-2.5">
            <Switch id="seg-centre" checked={!!s.centre} onCheckedChange={(v) => onChange({ centre: v })} />
            <Label htmlFor="seg-centre" className="font-normal">
              Centred
            </Label>
            <Tooltip>
              <TooltipTrigger asChild>
                <span className="text-xs text-muted-foreground underline decoration-dotted">auto font</span>
              </TooltipTrigger>
              <TooltipContent className="max-w-xs">Centred text is proportional; the sign picks the largest font that fits.</TooltipContent>
            </Tooltip>
          </div>
          <div className="grid gap-2">
            <Label>Font</Label>
            <Select value={s.font ?? "same"} onValueChange={(v) => onChange({ font: v === "same" ? null : v })} disabled={!!s.centre}>
              <SelectTrigger className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="same">{s.centre ? "Largest that fits" : "As before"}</SelectItem>
                {(assets?.sign_fonts ?? []).map((f) => (
                  <SelectItem key={f} value={f}>
                    {f.replace(".png", "")}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>
        </div>
      </div>
      <div className="grid content-start gap-5">
        <div className="grid gap-2">
          <Label>Comes in from</Label>
          <ToggleGroup type="single" variant="outline" value={s.entry} onValueChange={(v) => v && onChange({ entry: v })}>
            {(assets?.markup_entry ?? []).map((e) => {
              const I = ENTRY_ICON[e] ?? ArrowLeft
              return (
                <ToggleGroupItem key={e} value={e} aria-label={`From ${e}`} className="px-3 data-[state=on]:bg-primary data-[state=on]:text-primary-foreground">
                  <Tooltip>
                    <TooltipTrigger asChild>
                      <span className="grid size-full place-items-center">
                        <I />
                      </span>
                    </TooltipTrigger>
                    <TooltipContent>From the {e}</TooltipContent>
                  </Tooltip>
                </ToggleGroupItem>
              )
            })}
          </ToggleGroup>
        </div>
        <SliderRow label="Entry speed" value={s.entry_speed ?? 40} unit={`${s.entry_speed ?? 40} px/s`} min={1} max={255} onChange={(v) => onChange({ entry_speed: v })} />
        <SliderRow
          label="Hold"
          value={hold}
          unit={hold ? `${(hold / 10).toFixed(1)} s` : "Scroll through"}
          min={0}
          max={100}
          onChange={(v) => onChange({ hold: v, exit: v ? (s.exit ?? "left") : s.exit, exit_speed: v ? (s.exit_speed ?? 40) : s.exit_speed })}
        />
        <div className={cn("grid gap-2", !hold && "opacity-50")}>
          <Label>Leaves to</Label>
          <ToggleGroup type="single" variant="outline" value={s.exit ?? "left"} onValueChange={(v) => v && onChange({ exit: v })} disabled={!hold}>
            {(assets?.markup_exit ?? []).map((e) => {
              const I = EXIT_ICON[e] ?? ArrowLeft
              return (
                <ToggleGroupItem key={e} value={e} aria-label={`To ${e}`} className="px-3 data-[state=on]:bg-primary data-[state=on]:text-primary-foreground">
                  <I />
                </ToggleGroupItem>
              )
            })}
          </ToggleGroup>
        </div>
        <SliderRow label="Exit speed" value={s.exit_speed ?? 40} unit={`${s.exit_speed ?? 40} px/s`} min={1} max={255} onChange={(v) => onChange({ exit_speed: v })} disabled={!hold} />
      </div>
    </div>
  )
}

export function LinesPage() {
  const { data: assets } = useAssets()
  const { data: cfg } = useConfig()
  const [segs, setSegs] = useState<Seg[]>(load)
  const [sel, setSel] = useState(() => segs[0]?.id ?? 0)
  const [raw, setRaw] = useState<string | null>(null)
  const pal = useMemo(() => assets?.palette ?? {}, [assets])
  const { W, H } = usePanel()

  useEffect(() => {
    try {
      localStorage.setItem(KEY, JSON.stringify(segs.map(({ id: _id, ...s }) => s)))
    } catch {
      /* ignore */
    }
  }, [segs])

  const body = useMemo(
    () => ({
      segments: segs.map(({ id: _id, ...s }) => {
        if (s.include !== undefined) return { include: s.include }
        const o = { ...s }
        if (!o.hold) {
          o.exit = null
          o.exit_speed = null
        }
        return o
      }),
    }),
    [segs],
  )
  const debounced = useDebounced(body, 300)
  const q = useQuery({
    queryKey: ["markup", debounced, cfg?.panel.width, cfg?.panel.height],
    queryFn: () => api.markup({ ...debounced, preview: false }),   // bytes only: the preview runs here
    enabled: !!cfg,
    placeholderData: keepPreviousData,
    staleTime: Infinity,
  })
  const r = q.data
  const sign = useSign()
  // the preview: the sign's own text line, emulated in the browser (@marquee/sign-emu) on the exact bytes
  const emu = useMemo(() => {
    if (!sign.data || !r?.ok || !r.bytes) return null
    const data = Uint8Array.from(r.bytes.split(" ").filter(Boolean).map((h) => parseInt(h, 16)))
    const table = new Uint8Array(256)
    table.set(data.subarray(0, 255))
    const { frames, states } = textlineFrames(table, sign.data.fonts, W, H, 0, 2400)
    return { frames: toFrames(frames, W, H), done: states[states.length - 1] === 4, hang: states[states.length - 1] === -1 }
  }, [sign.data, r, W, H])
  const total = segs.reduce((a, s) => a + seconds(s, W), 0)
  const current = segs.find((s) => s.id === sel) ?? segs[0]
  const idx = segs.findIndex((s) => s.id === current?.id)

  const patch = (id: number, p: Partial<MarkupSegment>) => setSegs(segs.map((s) => (s.id === id ? { ...s, ...p } : s)))
  const add = (include = false) => {
    const last = [...segs].reverse().find((s) => s.include === undefined) ?? withId(DEFAULT[0])
    const n = include ? withId({ include: 1 }) : withId({ ...last, id: undefined, text: "TEXT" } as MarkupSegment)
    const at = idx + 1
    setSegs([...segs.slice(0, at), n, ...segs.slice(at)])
    setSel(n.id)
  }
  const move = (d: number) => {
    const j = idx + d
    if (j < 0 || j >= segs.length) return
    const s = [...segs]
    ;[s[idx], s[j]] = [s[j], s[idx]]
    setSegs(s)
  }
  const decode = async () => {
    if (raw == null) return
    try {
      const res = await api.markup({ raw, preview: false })
      const next = res.segments.map((s) => withId(s.include !== undefined ? s : { ...s, fg: colourName(s.fg, pal), bg: colourName(s.bg, pal) }))
      setSegs(next)
      setSel(next[0]?.id ?? 0)
      setRaw(null)
    } catch {
      /* the error shows in the result below */
    }
  }

  const record = r?.record
  return (
    <div className="mx-auto w-full max-w-6xl space-y-6 px-4 py-6 sm:px-6 lg:px-8">
      <Stage
        frames={emu?.frames}
        busy={q.isFetching}
        className="sticky top-16 z-10 bg-background/95 backdrop-blur supports-[backdrop-filter]:bg-background/80"
        actions={
          <SendDialog
            payload={body}
            description="The line goes out as one RS-485 record, the exact bytes below."
            warning={record && !record.scrolls ? "This fits the window, so the sign shows the codes as glyphs instead of using them. Make it longer." : undefined}
          >
            <Button size="sm" className="ml-1">
              <Send />
              Send to sign
            </Button>
          </SendDialog>
        }
      />

      <section className="space-y-3">
        <div className="flex items-center justify-between gap-3">
          <h2 className="text-sm font-medium">Timeline</h2>
          <div className="flex items-center gap-1">
            <Button variant="ghost" size="sm" onClick={() => add(false)}>
              <Plus />
              Segment
            </Button>
            <Button variant="ghost" size="sm" onClick={() => add(true)}>
              <Repeat />
              Include a line
            </Button>
          </div>
        </div>
        <Card className="gap-0 rounded-xl p-1">
        <ToggleGroup
          type="single"
          variant="outline"
          value={String(current?.id ?? "")}
          onValueChange={(v) => v && setSel(+v)}
          aria-label="Segments"
          className="flex h-14 w-full gap-1"
        >
          {segs.map((s, i) => {
            const fg = s.include === undefined ? colourName(s.fg, pal) : ""
            const css = fg in pal ? rgb555ToCss(pal[fg]) : "#888"
            return (
              <ToggleGroupItem
                key={s.id}
                value={String(s.id)}
                style={{ flexGrow: seconds(s, W), flexBasis: 0 }}
                className="h-full min-w-16 shrink flex-col items-start justify-center gap-0.5 overflow-hidden rounded-lg px-3 text-left data-[state=on]:border-foreground/60 data-[state=on]:bg-accent"
              >
                <span className="flex w-full items-center gap-1.5 text-xs text-muted-foreground tabular-nums">
                  {s.include === undefined && <span className="size-2 shrink-0 rounded-full" style={{ background: css, boxShadow: `0 0 6px ${css}` }} />}
                  {i + 1}
                  <span className="ml-auto">{seconds(s, W).toFixed(1)} s</span>
                </span>
                <span className="w-full truncate text-sm font-medium">{s.include !== undefined ? `Line ${s.include}` : s.text || "(empty)"}</span>
              </ToggleGroupItem>
            )
          })}
        </ToggleGroup>
        </Card>
        <p className="text-right text-xs text-muted-foreground tabular-nums">about {total.toFixed(1)} s</p>
      </section>

      {current && (
        <Card>
          <CardContent className="grid gap-6">
            <div className="flex items-center gap-2">
              <Badge variant="secondary" className="tabular-nums">
                Segment {idx + 1}
              </Badge>
              <div className="ml-auto flex items-center gap-1">
                <Button variant="ghost" size="icon-sm" aria-label="Move earlier" onClick={() => move(-1)} disabled={idx <= 0}>
                  <ChevronLeft />
                </Button>
                <Button variant="ghost" size="icon-sm" aria-label="Move later" onClick={() => move(1)} disabled={idx >= segs.length - 1}>
                  <ChevronRight />
                </Button>
                <Button
                  variant="ghost"
                  size="icon-sm"
                  aria-label="Delete segment"
                  disabled={segs.length <= 1}
                  onClick={() => {
                    const rest = segs.filter((s) => s.id !== current.id)
                    setSegs(rest)
                    setSel(rest[Math.max(0, idx - 1)]?.id ?? 0)
                  }}
                >
                  <Trash2 />
                </Button>
              </div>
            </div>
            <SegmentEditor s={current} onChange={(p) => patch(current.id, p)} />
          </CardContent>
        </Card>
      )}

      {q.error && (
        <Alert variant="destructive">
          <TriangleAlert />
          <AlertDescription>{q.error.message}</AlertDescription>
        </Alert>
      )}
      {r && (
        <div className="grid gap-2">
          {r.ok === false && (
            <Alert variant="destructive">
              <TriangleAlert />
              <AlertDescription>{r.error}</AlertDescription>
            </Alert>
          )}
          {record && (
            <Alert className={record.scrolls ? "" : "border-amber-500/30 text-amber-600 dark:text-amber-400"}>
              {record.scrolls ? <CircleCheck /> : <TriangleAlert />}
              <AlertDescription className={record.scrolls ? "" : "text-amber-700/90 dark:text-amber-300/80"}>
                {record.scrolls
                  ? `Plays as a message line on the sign (${record.width} px in ${record.font}, wider than the window).`
                  : `Fits the window (${record.width} px), so as a live record the sign draws it plainly. Make it longer, or use it as an attract line.`}
              </AlertDescription>
            </Alert>
          )}
          {(r.warnings ?? []).map((w) => (
            <Alert key={w} className="border-amber-500/30 text-amber-600 dark:text-amber-400">
              <TriangleAlert />
              <AlertDescription className="text-amber-700/90 dark:text-amber-300/80">{w}</AlertDescription>
            </Alert>
          ))}
          {emu?.hang && (
            <Alert variant="destructive">
              <TriangleAlert />
              <AlertDescription>The firmware hangs on this line.</AlertDescription>
            </Alert>
          )}
        </div>
      )}

      <Collapsible>
        <CollapsibleTrigger asChild>
          <Button variant="ghost" size="sm" className="group -ml-2 text-muted-foreground">
            <ChevronDown className="transition-transform group-data-[state=closed]:-rotate-90" />
            Bytes
            {r && <span className="tabular-nums">· {r.length} of 255</span>}
          </Button>
        </CollapsibleTrigger>
        <CollapsibleContent className="grid gap-4 pt-3">
          <div className="grid gap-2">
            <Label htmlFor="raw">Message line (hex)</Label>
            <Textarea
              id="raw"
              spellCheck={false}
              className="min-h-20 font-mono text-xs"
              value={raw ?? r?.bytes ?? ""}
              onChange={(e) => setRaw(e.target.value)}
            />
            <div className="flex gap-2">
              <Button variant="outline" size="sm" onClick={decode} disabled={raw == null}>
                Decode into segments
              </Button>
              {raw != null && (
                <Button variant="ghost" size="sm" onClick={() => setRaw(null)}>
                  Revert
                </Button>
              )}
            </div>
          </div>
          <div className="grid gap-2">
            <Label htmlFor="frame">RS-485 frame</Label>
            <Textarea id="frame" readOnly spellCheck={false} className="min-h-16 font-mono text-xs text-muted-foreground" value={r?.frame ?? r?.frame_error ?? ""} />
          </div>
        </CollapsibleContent>
      </Collapsible>
    </div>
  )
}
