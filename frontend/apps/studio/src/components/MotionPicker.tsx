import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { Slider } from "@marquee/ui/components/ui/slider"
import { Label } from "@marquee/ui/components/ui/label"
import { Card, CardContent } from "@marquee/ui/components/ui/card"
import { MoveLeft, Square, Zap, type LucideIcon } from "@marquee/ui/lib/icons"
import { SAFE_FLASH, useComposer, type Motion } from "@/state/composer"

const MOTIONS: [Motion, string, string, LucideIcon][] = [
  ["scroll", "Scroll", "Glides across", MoveLeft],
  ["static", "Still", "Stays centred", Square],
  ["flash", "Flash", "Blinks on and off", Zap],
]

function Row({ label, value, children }: { label: string; value: string; children: React.ReactNode }) {
  return (
    <div className="grid gap-3">
      <div className="flex items-center justify-between">
        <Label className="text-sm font-normal text-muted-foreground">{label}</Label>
        <span className="text-sm tabular-nums">{value}</span>
      </div>
      {children}
    </div>
  )
}

export function MotionPicker() {
  const { c, set } = useComposer()
  return (
    <div className="grid gap-8 lg:grid-cols-[minmax(0,1fr)_minmax(0,22rem)]">
      <ToggleGroup
        type="single"
        variant="outline"
        value={c.motion}
        onValueChange={(v) => {
          if (!v) return
          const m = v as Motion
          // switching to Flash starts from a safe 1 s cycle
          set(m === "flash" && c.motion !== "flash" ? { motion: m, flashOnMs: SAFE_FLASH.on, flashOffMs: SAFE_FLASH.off } : { motion: m })
        }}
        spacing={3}
        aria-label="Motion"
        className="grid w-full grid-cols-3 gap-3"
      >
        {MOTIONS.map(([v, name, sub, Icon]) => (
          <ToggleGroupItem
            key={v}
            value={v}
            className="flex h-auto w-full flex-col items-start gap-3 rounded-xl p-4 text-left data-[state=on]:border-foreground/60 data-[state=on]:bg-accent/60"
          >
            <Icon className="size-5" />
            <span className="grid gap-0.5">
              <span className="text-sm font-medium">{name}</span>
              <span className="text-xs font-normal text-muted-foreground">{sub}</span>
            </span>
          </ToggleGroupItem>
        ))}
      </ToggleGroup>
      <Card className="py-5">
        <CardContent className="grid content-start gap-6 px-5">
        {c.motion === "scroll" && (
          <Row label="Speed" value={`${c.textRate} px / frame`}>
            <Slider min={1} max={8} step={1} value={[c.textRate]} onValueChange={([v]) => set({ textRate: v })} aria-label="Speed" />
          </Row>
        )}
        {c.motion === "static" && <p className="text-sm text-muted-foreground">Centred on the panel. Animations keep playing.</p>}
        {c.motion === "flash" && (
          <>
            <Row label="On" value={`${c.flashOnMs} ms`}>
              <Slider min={25} max={2000} step={25} value={[c.flashOnMs]} onValueChange={([v]) => set({ flashOnMs: v })} aria-label="Flash on" />
            </Row>
            <Row label="Off" value={`${c.flashOffMs} ms`}>
              <Slider min={25} max={2000} step={25} value={[c.flashOffMs]} onValueChange={([v]) => set({ flashOffMs: v })} aria-label="Flash off" />
            </Row>
          </>
        )}
        </CardContent>
      </Card>
    </div>
  )
}
