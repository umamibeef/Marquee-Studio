import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { useAssets, usePanel, type Assets } from "@/lib/api"
import { toSpec, useComposer, type Composer } from "@/state/composer"
import { useRendered } from "@/lib/emu"
import type { Spec } from "@marquee/sign-emu"
import { Thumb, useActive, useSeen } from "./Thumb"

interface Preset {
  id: string
  name: string
  patch: Partial<Composer>
  /** fill / background animation, found by its authoring name prefix (the FLC's own name, e.g. RBOW-RYG) */
  fillAnim?: string
  bgAnim?: string
}

// Built from the sign's own colours and effect animations.
const PRESETS: Preset[] = [
  { id: "classic", name: "Classic red", patch: { fill: "Red", background: "Off", outline: "Off" } },
  { id: "gold", name: "Gold", patch: { fill: "Gold", background: "Off", outline: "Red", outlineStyle: "Thin" } },
  { id: "rainbow", name: "Rainbow", fillAnim: "RBOW-RYG", patch: { background: "Off", outline: "Off" } },
  { id: "glitter", name: "Glitter", fillAnim: "GLTR-YR", patch: { background: "Off", outline: "Off" } },
  { id: "calm", name: "Calm blue", fillAnim: "WAVE-GCB", patch: { background: "Off", outline: "Off" } },
  { id: "diamond", name: "Diamonds", bgAnim: "DMOND-BG", patch: { fill: "White", outline: "Black", outlineStyle: "Thick" } },
]

function findAnim(a: Assets | undefined, author: string | undefined) {
  if (!a || !author) return undefined
  return Object.entries(a.effect_names).find(([, n]) => n.toUpperCase() === author.toUpperCase())?.[0]
}

export function presetPatch(p: Preset, a: Assets | undefined): Partial<Composer> | null {
  const patch = { ...p.patch }
  if (p.fillAnim) {
    const f = findAnim(a, p.fillAnim)
    if (!f) return null
    patch.fill = f
  }
  if (p.bgAnim) {
    const b = findAnim(a, p.bgAnim)
    if (!b) return null
    patch.background = b
  }
  return patch
}

function matches(c: Composer, patch: Partial<Composer>) {
  return (Object.keys(patch) as (keyof Composer)[]).every((k) => k === "outlineStyle" || c[k] === patch[k])
}

function PresetCard({ p, patch, sample }: { p: Preset; patch: Partial<Composer>; sample: Composer }) {
  const { data: assets } = useAssets()
  const { W, H } = usePanel()
  const [ref, seen] = useSeen<HTMLDivElement>()
  const [active, hover] = useActive()
  const spec = toSpec({ ...sample, ...patch, text: "ABCdef", value: "", motion: "static" }, assets?.palette)
  const q = useRendered(["preset", spec], spec as Spec, W, H, seen && !!assets)
  return (
    <div ref={ref} className="w-full">
      <ToggleGroupItem
        value={p.id}
        aria-label={p.name}
        className="flex h-auto w-full flex-col items-stretch gap-2 rounded-xl p-2 data-[state=on]:border-foreground/60 data-[state=on]:bg-accent/60"
        {...hover}
      >
        <Thumb frames={q.data} active={active} label={p.name} />
        <span className="px-0.5 text-left text-sm font-medium">{p.name}</span>
      </ToggleGroupItem>
    </div>
  )
}

/** Style presets as cards with live mini previews. */
export function Presets() {
  const { c, set } = useComposer()
  const { data: assets } = useAssets()
  const items = PRESETS.map((p) => ({ p, patch: presetPatch(p, assets) })).filter(
    (x): x is { p: Preset; patch: Partial<Composer> } => !!x.patch,
  )
  const current = items.find((x) => matches(c, x.patch))?.p.id ?? ""
  return (
    <ToggleGroup
      type="single"
      variant="outline"
      value={current}
      onValueChange={(v) => {
        const x = items.find((i) => i.p.id === v)
        if (x) set(x.patch)
      }}
      spacing={3}
      aria-label="Style"
      className="grid w-full grid-cols-2 gap-3 md:grid-cols-3 xl:grid-cols-6"
    >
      {items.map(({ p, patch }) => (
        <PresetCard key={p.id} p={p} patch={patch} sample={c} />
      ))}
    </ToggleGroup>
  )
}
