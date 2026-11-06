// The message's formatting palette, as in a word processor: each control applies to the selected letters, or to the
// whole message when nothing is selected, and shows the state of what it would change. Fill, motion and on/off time
// can be per letter (in-text commands, state/composer.tsx withEffects); background, outline and font belong to the
// whole line on the sign, and say so.
import { useRef, useState, type ReactNode } from "react"
import { Button } from "@marquee/ui/components/ui/button"
import { Input } from "@marquee/ui/components/ui/input"
import { Label } from "@marquee/ui/components/ui/label"
import { Popover, PopoverContent, PopoverTrigger } from "@marquee/ui/components/ui/popover"
import { Separator } from "@marquee/ui/components/ui/separator"
import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import {
  Baseline, ChevronDown, EyeOff, Highlighter, ImagePlus, Loader2, MoveVertical, Redo2, RemoveFormatting, Square, Timer, Type, Undo2, Vibrate, Waves, type LucideIcon,
} from "@marquee/ui/lib/icons"
import { rgb555ToCss, useAssets, type Assets } from "@/lib/api"
import { IMAGE_TYPES } from "@/lib/useImportGif"
import { useComposer, type CharFx } from "@/state/composer"
import { AnimationPicker, type AnimationChoice } from "./AnimationPicker"
import { ColourPicker } from "./ColourPicker"
import { FontPicker, fontTitle } from "./FontPicker"
import { OutlinePicker } from "./OutlinePicker"

const MOTION_ITEMS: { id: number; name: string; hint: string; icon: LucideIcon }[] = [
  { id: 1, name: "Wave", hint: "Wave: the letters ripple along a moving wave", icon: Waves },
  { id: 2, name: "Bounce", hint: "Bounce: each letter hops in turn", icon: MoveVertical },
  { id: 3, name: "Shake", hint: "Shake: the letters jitter", icon: Vibrate },
  { id: 4, name: "Blink", hint: "Blink: the letters blink on and off", icon: EyeOff },
]

/** The fills that are not palette colours or animation files. */
const BUILT_IN_FILLS: AnimationChoice[] = [
  { name: "Red/green/yellow wipe", label: "Wipe" },
  { name: "0x8002", label: "Stripes" },
]

/** The effect word of a fill, for a per-letter fill; null when the sign can only do it for a whole line. */
export function fillWord(name: string, a: Assets | undefined): number | null {
  if (a && name in a.palette) return a.palette[name]
  if (name === "Red/green/yellow wipe") return 0x8001
  if (name.toLowerCase().startsWith("0x")) return parseInt(name, 16) & 0xffff
  return a?.animation_info[name]?.slot ?? null
}

/** The name the palette knows an effect word by (the inverse of fillWord). */
export function fillName(word: number, a: Assets | undefined): string {
  for (const [n, w] of Object.entries(a?.palette ?? {})) if (w === word) return n
  if (word === 0x8001) return "Red/green/yellow wipe"
  if (word === 0x8002) return "0x8002"
  for (const [n, i] of Object.entries(a?.animation_info ?? {})) if (i.slot === word) return n
  return ""
}

const MIXED_CSS = "repeating-linear-gradient(135deg, var(--muted-foreground) 0 2px, transparent 2px 4px)"
const ANIMATED_CSS = "linear-gradient(90deg, #ef4444, #eab308, #22c55e, #3b82f6, #a855f7)"

function barCss(value: string, a: Assets | undefined): string {
  if (!value) return MIXED_CSS
  if (a && value in a.palette) return value === "Off" ? "var(--border)" : rgb555ToCss(a.palette[value])
  return ANIMATED_CSS
}

function PaletteMenu({
  icon: Icon, label, hint, bar, children, width, wide, onOpen, text, onClosed,
}: {
  icon: LucideIcon
  label: string
  hint: string
  /** the current value as a bar under the icon, as a font-colour button shows it */
  bar?: string
  text?: string
  width?: string
  wide?: boolean
  children: (close: () => void) => ReactNode
  onOpen: (open: boolean) => void
  onClosed: () => void
}) {
  const [open, setOpen] = useState(false)
  const change = (v: boolean) => {
    setOpen(v)
    onOpen(v)
  }
  return (
    <Popover open={open} onOpenChange={change}>
      <Tooltip>
        <TooltipTrigger asChild>
          <PopoverTrigger asChild>
            <Button variant="ghost" size="sm" aria-label={label} className="gap-1 px-2">
              <span className="relative flex size-4 items-start justify-center">
                <Icon className="size-3.5" />
                {bar && <span data-slot="value-bar" className="absolute inset-x-0 -bottom-0.5 h-[3px] rounded-[1px]" style={{ background: bar }} />}
              </span>
              {text && <span className="max-w-32 truncate">{text}</span>}
              <ChevronDown className="size-3 opacity-60" />
            </Button>
          </PopoverTrigger>
        </TooltipTrigger>
        <TooltipContent>{hint}</TooltipContent>
      </Tooltip>
      <PopoverContent
        align="start"
        collisionPadding={12}
        className={`${width ?? "w-96"} ${wide ? "max-h-[70vh] overflow-y-auto" : ""} space-y-4`}
        onOpenAutoFocus={(e) => e.preventDefault()}
        onCloseAutoFocus={(e) => {
          e.preventDefault()
          onClosed()
        }}
      >
        {children(() => change(false))}
      </PopoverContent>
    </Popover>
  )
}

function Note({ children }: { children: ReactNode }) {
  return <p className="text-xs text-muted-foreground">{children}</p>
}

function Heading({ children }: { children: ReactNode }) {
  return <Label className="text-xs text-muted-foreground">{children}</Label>
}

/** Whole-line controls with letters selected: say that the selection doesn't narrow them. */
const WHOLE_LINE_NOTE = "Applies to the whole message: the sign draws this per line, not per letter."

export function FormatPalette({
  fx, sel, onFx, onClear, onHold, onRestore, onGif, gifBusy,
}: {
  fx: CharFx[]
  /** the selection [start, end); empty: the whole message */
  sel: [number, number]
  /** apply to the selection, or to every letter when nothing is selected */
  onFx: (patch: Partial<CharFx>) => void
  onClear: () => void
  /** a menu opened or closed: the box keeps showing the selection meanwhile */
  onHold: (open: boolean) => void
  onRestore: () => void
  /** a GIF to put at the caret */
  onGif: (files: File[]) => void
  gifBusy: boolean
}) {
  const gifInput = useRef<HTMLInputElement>(null)
  const { c, set, undo, redo, canUndo, canRedo } = useComposer()
  const { data: assets } = useAssets()
  const mod = /Mac|iPhone|iPad/.test(navigator.platform) ? "⌘" : "Ctrl+"
  const [a, b] = sel
  const selected = b > a
  const scope = selected ? fx.slice(a, b) : fx

  // what the scope has: one value, or "" when mixed
  const fills = new Set(scope.map((e) => e.f))
  const fillValue = fills.size > 1 ? "" : (() => {
    const f = [...fills][0] ?? null
    return f === null ? c.fill : fillName(f, assets)
  })()
  const motions = new Set(scope.map((e) => e.m))
  const motion = motions.size === 1 ? [...motions][0] : -1
  const hasFormat = scope.some((e) => e.m !== 0 || e.f !== null || e.o !== null)

  const pickFill = (name: string) => {
    if (!selected) {
      set({ fill: name, fx: fx.map((e) => ({ ...e, f: null })) })
      return
    }
    const word = fillWord(name, assets)
    if (word !== null) onFx({ f: word })
  }
  const anims = assets?.animations ?? []
  const perLetterAnims = anims.filter((n) => assets?.animation_info[n]?.slot != null)
  const menu = { onOpen: onHold, onClosed: onRestore }
  const on = scope[0]?.o ?? null
  const fillColours = Object.keys(assets?.palette ?? {}).filter((n) => n !== "Off")

  return (
    <div className="flex flex-wrap items-center gap-1 rounded-lg border bg-card p-1" role="toolbar" aria-label="Formatting"
         onMouseDown={(e) => { if (!(e.target instanceof HTMLInputElement)) e.preventDefault() }}>
      <Tooltip>
        <TooltipTrigger asChild>
          {/* a span: a disabled button shows no tooltip */}
          <span>
            <Button variant="ghost" size="icon-sm" aria-label="Undo" disabled={!canUndo} onClick={undo}>
              <Undo2 className="size-3.5" />
            </Button>
          </span>
        </TooltipTrigger>
        <TooltipContent>Undo ({mod}Z)</TooltipContent>
      </Tooltip>
      <Tooltip>
        <TooltipTrigger asChild>
          <span>
            <Button variant="ghost" size="icon-sm" aria-label="Redo" disabled={!canRedo} onClick={redo}>
              <Redo2 className="size-3.5" />
            </Button>
          </span>
        </TooltipTrigger>
        <TooltipContent>Redo ({mod === "⌘" ? "⇧⌘Z" : "Ctrl+Y"})</TooltipContent>
      </Tooltip>

      <Separator orientation="vertical" className="mx-1 !h-6" />

      <PaletteMenu {...menu} icon={Type} label="Font" hint="Font of the whole message" text={fontTitle(c.font, assets?.font_info[c.font])} width="w-[min(44rem,calc(100vw-2rem))]" wide>
        {(close) => (
          <>
            {selected && <Note>{WHOLE_LINE_NOTE}</Note>}
            <FontPicker value={c.font} onChange={(v) => { set({ font: v }); close() }} sample="ABCdef" />
          </>
        )}
      </PaletteMenu>

      <Separator orientation="vertical" className="mx-1 !h-6" />

      <PaletteMenu {...menu} icon={Baseline} label="Fill" bar={barCss(fillValue, assets)} width="w-[min(40rem,calc(100vw-2rem))]" wide
                   hint={selected ? "Fill of the selected letters" : "Fill of the whole message"}>
        {(close) => (
          <>
            <div className="space-y-2">
              <Heading>Colour</Heading>
              <ColourPicker label="Fill colour" value={fillValue} colours={fillColours} onChange={(v) => { pickFill(v); close() }} />
            </div>
            <div className="space-y-2">
              <Heading>Animated</Heading>
              <AnimationPicker role="fg" autoPlay value={fillValue} font={c.font} extra={BUILT_IN_FILLS}
                               only={selected ? perLetterAnims : undefined} className="grid-cols-2 sm:grid-cols-3 lg:grid-cols-3 xl:grid-cols-3"
                               onChange={(v) => { pickFill(v); close() }} />
            </div>
            {selected && (
              <Button variant="ghost" size="sm" className="w-full" onClick={() => { onFx({ f: null }); close() }}>
                The message's own fill
              </Button>
            )}
          </>
        )}
      </PaletteMenu>

      <PaletteMenu {...menu} icon={Highlighter} label="Background" bar={barCss(c.background, assets)} width="w-[min(40rem,calc(100vw-2rem))]" wide hint="Background of the whole message">
        {(close) => (
          <>
            {selected && <Note>{WHOLE_LINE_NOTE}</Note>}
            <div className="space-y-2">
              <Heading>Colour</Heading>
              <ColourPicker label="Background colour" value={c.background} onChange={(v) => { set({ background: v }); close() }} />
            </div>
            <div className="space-y-2">
              <Heading>Animated</Heading>
              <AnimationPicker role="bg" autoPlay value={c.background} font={c.font}
                               className="grid-cols-2 sm:grid-cols-3 lg:grid-cols-3 xl:grid-cols-3"
                               onChange={(v) => { set({ background: v }); close() }} />
            </div>
          </>
        )}
      </PaletteMenu>

      <PaletteMenu {...menu} icon={Square} label="Outline" bar={c.outline === "Off" ? undefined : barCss(c.outline === "Black" ? "Off" : c.outline, assets)} width="w-[min(30rem,calc(100vw-2rem))]" hint="Outline of the whole message">
        {() => (
          <>
            {selected && <Note>{WHOLE_LINE_NOTE}</Note>}
            <OutlinePicker />
          </>
        )}
      </PaletteMenu>

      <Separator orientation="vertical" className="mx-1 !h-6" />

      <ToggleGroup type="single" size="sm" variant="default" value={motion > 0 ? String(motion) : ""}
                   onValueChange={(v) => onFx({ m: v === "" ? 0 : Number(v) })} aria-label="Letter motion">
        {MOTION_ITEMS.map((m) => (
          <ToggleGroupItem key={m.id} value={String(m.id)} aria-label={m.name} className="px-2 data-[state=on]:bg-primary data-[state=on]:text-primary-foreground">
            {/* the tooltip trigger is inside the item: on the item it would overwrite the item's data-state */}
            <Tooltip>
              <TooltipTrigger asChild>
                <span className="flex items-center"><m.icon className="size-3.5" /></span>
              </TooltipTrigger>
              <TooltipContent>{m.hint}</TooltipContent>
            </Tooltip>
          </ToggleGroupItem>
        ))}
      </ToggleGroup>

      <PaletteMenu {...menu} icon={Timer} label="On and off" width="w-72"
                   hint={selected ? "Run the selected letters' effects for a while, then rest" : "Run the message's effects for a while, then rest"}>
        {() => <OnOff value={on} onPick={(o) => onFx({ o })} />}
      </PaletteMenu>

      <Separator orientation="vertical" className="mx-1 !h-6" />

      <Tooltip>
        <TooltipTrigger asChild>
          <Button variant="ghost" size="icon-sm" aria-label="Clear formatting" disabled={!hasFormat} onClick={onClear}>
            <RemoveFormatting className="size-3.5" />
          </Button>
        </TooltipTrigger>
        <TooltipContent>{selected ? "Clear the selected letters' own effects" : "Clear every letter's own effects"}</TooltipContent>
      </Tooltip>

      <Separator orientation="vertical" className="mx-1 !h-6" />

      <Tooltip>
        <TooltipTrigger asChild>
          <Button variant="ghost" size="sm" className="gap-1.5 px-2" aria-label="Insert GIF" disabled={gifBusy} onClick={() => gifInput.current?.click()}>
            {gifBusy ? <Loader2 className="size-3.5 animate-spin" /> : <ImagePlus className="size-3.5" />}
            GIF
          </Button>
        </TooltipTrigger>
        <TooltipContent>Put an animated GIF in the text, at the caret. You can also drop one on the message.</TooltipContent>
      </Tooltip>
      <input ref={gifInput} data-testid="gif-input" type="file" hidden multiple accept={IMAGE_TYPES}
             onChange={(e) => { onGif([...(e.target.files ?? [])]); e.target.value = "" }} />

      <span data-testid="format-scope" className="ml-auto px-2 text-xs text-muted-foreground">
        {selected ? `${b - a} character${b - a === 1 ? "" : "s"} selected` : "Whole message"}
      </span>
    </div>
  )
}

function OnOff({ value, onPick }: { value: [number, number] | null; onPick: (o: [number, number] | null) => void }) {
  const [on, off] = value ?? [3, 3]
  const tenths = (v: string) => Math.max(0, Math.min(255, Math.round(Number(v) * 10) || 0))
  return (
    <>
      <div className="grid grid-cols-2 gap-3">
        <div className="space-y-1.5">
          <Label htmlFor="fx-on" className="text-xs text-muted-foreground">On (seconds)</Label>
          <Input id="fx-on" type="number" min={0.1} max={25.5} step={0.1} defaultValue={on / 10}
                 onChange={(e) => onPick([tenths(e.target.value) || 1, off])} />
        </div>
        <div className="space-y-1.5">
          <Label htmlFor="fx-off" className="text-xs text-muted-foreground">Off (seconds)</Label>
          <Input id="fx-off" type="number" min={0} max={25.5} step={0.1} defaultValue={off / 10}
                 onChange={(e) => onPick([on || 1, tenths(e.target.value)])} />
        </div>
      </div>
      <Button variant="ghost" size="sm" className="w-full" onClick={() => onPick(null)}>
        Always on
      </Button>
    </>
  )
}
