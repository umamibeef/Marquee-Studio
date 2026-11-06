import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { useAssets } from "@/lib/api"
import { useComposer } from "@/state/composer"
import { ColourPicker } from "./ColourPicker"

const OUTLINE_PALETTE: Record<string, string> = {
  Black: "Off",
  Red: "Red",
  Green: "Green",
  Yellow: "Yellow",
  Blue: "Blue",
  Cyan: "Cyan",
  Magenta: "Magenta",
  White: "White",
}

/** The sign's 8 outline colours and 3 styles: Compose's outline, or (with value/onChange) any other. */
export function OutlinePicker(props: { value?: { outline: string; outlineStyle: string }; onChange?: (v: { outline: string; outlineStyle: string }) => void }) {
  const composer = useComposer()
  const c = props.value ?? composer.c
  const set = (v: Partial<{ outline: string; outlineStyle: string }>) =>
    props.onChange ? props.onChange({ outline: c.outline, outlineStyle: c.outlineStyle, ...v }) : composer.set(v)
  const { data: assets } = useAssets()
  const styles = assets?.outline_styles ?? ["Thick", "Thin", "Drop Shadow"]
  const on = c.outline !== "Off"
  return (
    <div className="flex flex-wrap items-center gap-6">
      <ToggleGroup
        type="single"
        variant="outline"
        value={on ? c.outlineStyle : "none"}
        onValueChange={(v) => {
          if (!v) return
          if (v === "none") set({ outline: "Off" })
          else set({ outlineStyle: v, outline: on ? c.outline : "Red" })
        }}
        aria-label="Outline style"
      >
        <ToggleGroupItem value="none" className="px-3 data-[state=on]:bg-primary data-[state=on]:text-primary-foreground">None</ToggleGroupItem>
        {styles.map((s) => (
          <ToggleGroupItem key={s} value={s} className="px-3 data-[state=on]:bg-primary data-[state=on]:text-primary-foreground">
            {s}
          </ToggleGroupItem>
        ))}
      </ToggleGroup>
      {on && (
        <ColourPicker
          label="Outline colour"
          value={c.outline === "Black" ? "Off" : c.outline}
          onChange={(v) => set({ outline: v === "Off" ? "Black" : v })}
          colours={Object.values(OUTLINE_PALETTE)}
        />
      )}
    </div>
  )
}
