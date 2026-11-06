import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import { rgb555ToCss, useAssets } from "@/lib/api"

/** The sign's palette as round swatches. */
export function ColourPicker({
  value,
  onChange,
  colours,
  label,
}: {
  value: string
  onChange: (v: string) => void
  /** names to offer (default: the whole palette) */
  colours?: string[]
  label: string
}) {
  const { data: assets } = useAssets()
  const pal = assets?.palette ?? {}
  const names = colours ?? Object.keys(pal)
  return (
    // Ghost (variant "default") on purpose: each item is a colour swatch; the selected one gets a ring instead.
    <ToggleGroup
      type="single"
      variant="default"
      value={names.includes(value) ? value : ""}
      onValueChange={(v) => v && onChange(v)}
      aria-label={label}
      spacing={1}
      className="flex flex-wrap gap-1.5"
    >
      {names.map((n) => {
        const css = n in pal ? rgb555ToCss(pal[n]) : "transparent"
        return (
          <ToggleGroupItem
            key={n}
            value={n}
            aria-label={n === "Off" ? "None (black)" : n}
            className="size-9 min-w-9 rounded-full p-1 data-[state=on]:bg-transparent data-[state=on]:ring-2 data-[state=on]:ring-foreground data-[state=on]:ring-offset-2 data-[state=on]:ring-offset-background"
          >
            {/* the tooltip trigger is inside the item: on the item it would overwrite the item's data-state */}
            <Tooltip>
              <TooltipTrigger asChild>
                <span
                  className="block size-full rounded-full ring-1 ring-inset ring-white/10"
                  style={{
                    background: n === "Off" ? "repeating-linear-gradient(135deg,#1c1c20 0 4px,#0d0d10 4px 8px)" : css,
                    boxShadow: n === "Off" ? undefined : `0 0 10px -2px ${css}`,
                  }}
                />
              </TooltipTrigger>
              <TooltipContent>{n === "Off" ? "None (black)" : n}</TooltipContent>
            </Tooltip>
          </ToggleGroupItem>
        )
      })}
    </ToggleGroup>
  )
}
