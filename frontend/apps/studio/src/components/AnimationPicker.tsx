import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import { Badge } from "@marquee/ui/components/ui/badge"
import { useAssets, usePanel, type Assets } from "@/lib/api"
import { useRendered } from "@/lib/emu"
import type { Spec } from "@marquee/sign-emu"
import { cn } from "@marquee/ui/lib/utils"
import { Thumb, useActive, useSeen } from "./Thumb"

export function animLabel(a: Assets | undefined, name: string) {
  return a?.effect_names[name] ?? name.replace(/\.flc$/i, "")
}

/** A fill that is not a file: a built-in effect, by the name the renderer knows it by. */
export interface AnimationChoice { name: string; label: string }

function AnimItem({ name, label, role, font, selected, autoPlay, sample: text }: { name: string; label?: string; role: "fg" | "bg"; font: string; selected: boolean; autoPlay?: boolean; sample?: string }) {
  const { data: assets } = useAssets()
  const { W, H } = usePanel()
  const [ref, seen] = useSeen<HTMLDivElement>()
  const [active, hover] = useActive()
  const sample = text || "ABCdef"
  const base = { text: sample, font, mode: "static" }
  const q = useRendered(["fx", name, role, sample, font],
    (role === "fg" ? { ...base, foreground: name } : { ...base, background: name, colour: "Amber" }) as Spec,
    W, H, seen)
  const info = assets?.animation_info[name]
  const title = label ?? animLabel(assets, name)
  return (
    <div ref={ref} className="w-full">
      <ToggleGroupItem
        value={name}
        aria-label={title}
        className="flex h-auto w-full flex-col items-stretch gap-1.5 rounded-lg p-1.5 data-[state=on]:border-foreground/60 data-[state=on]:bg-accent/60"
        {...hover}
      >
        <Thumb frames={q.data} active={active || selected || !!autoPlay} label={title} />
        <span className="flex items-center justify-between gap-1 px-0.5 text-xs">
          {/* the tooltip trigger is inside the item: on the item it would overwrite the item's data-state */}
          <Tooltip>
            <TooltipTrigger asChild>
              <span className="truncate">{title}</span>
            </TooltipTrigger>
            <TooltipContent side="bottom">
              {label ? "Built into the sign" : name}
              {label ? "" : info?.slot != null ? ` · effect slot ${info.slot & 0x3fff}` : " · not an effect slot: previewed in a free slot"}
            </TooltipContent>
          </Tooltip>
          {!label && info?.slot == null && (
            <Badge variant="outline" className="h-4 px-1 text-[10px] font-normal text-muted-foreground">
              extra
            </Badge>
          )}
        </span>
      </ToggleGroupItem>
    </div>
  )
}

/** Every animation in the content folder as a thumbnail grid (plays on hover). */
export function AnimationPicker({
  role,
  value,
  onChange,
  font,
  only,
  autoPlay,
  sample,
  extra,
  className,
}: {
  role: "fg" | "bg"
  value: string
  onChange: (v: string) => void
  font: string
  /** offer only these (default: every animation in the content folder) */
  only?: string[]
  /** every preview plays (in a menu, where there is nothing to hover first) */
  autoPlay?: boolean
  /** the text the previews draw */
  sample?: string
  /** choices before the animation files */
  extra?: AnimationChoice[]
  className?: string
}) {
  const { data: assets } = useAssets()
  const anims = (assets?.animations ?? []).filter((a) => !only || only.includes(a))
  return (
    <ToggleGroup
      type="single"
      variant="outline"
      value={value}
      onValueChange={(v) => v && onChange(v)}
      spacing={2}
      className={cn("grid w-full gap-2", className ?? "grid-cols-2 sm:grid-cols-3 lg:grid-cols-4 xl:grid-cols-5")}
    >
      {extra?.map((x) => (
        <AnimItem key={x.name} name={x.name} label={x.label} role={role} font={font} selected={value === x.name} autoPlay={autoPlay} sample={sample} />
      ))}
      {anims.map((a) => (
        <AnimItem key={a} name={a} role={role} font={font} selected={value === a} autoPlay={autoPlay} sample={sample} />
      ))}
    </ToggleGroup>
  )
}
