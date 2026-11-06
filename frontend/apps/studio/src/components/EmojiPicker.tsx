import { useMemo, useRef, useState, type ReactNode } from "react"
import { toast } from "@marquee/ui/lib/toast"
import { Button } from "@marquee/ui/components/ui/button"
import { Input } from "@marquee/ui/components/ui/input"
import { Popover, PopoverContent, PopoverTrigger } from "@marquee/ui/components/ui/popover"
import { Command, CommandEmpty, CommandGroup, CommandInput, CommandItem, CommandList } from "@marquee/ui/components/ui/command"
import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import { Skeleton } from "@marquee/ui/components/ui/skeleton"
import {
  Apple,
  Clock,
  Dices,
  Flag,
  Heart,
  Lamp,
  Leaf,
  Plane,
  ImagePlus,
  Smile,
  Trash2,
  User,
  type LucideIcon,
} from "@marquee/ui/lib/icons"
import { api, useEmoji, useRefreshContent, type EmojiEntry } from "@/lib/api"

const GROUPS: [string, LucideIcon][] = [
  ["Recent", Clock],
  ["Smileys", Smile],
  ["People", User],
  ["Animals & nature", Leaf],
  ["Food & drink", Apple],
  ["Activities", Dices],
  ["Travel & places", Plane],
  ["Objects", Lamp],
  ["Symbols", Heart],
  ["Flags", Flag],
  ["Custom", ImagePlus],
]

const RECENT_KEY = "marquee-studio.emoji-recent.v1"

function loadRecent(): string[] {
  try {
    return JSON.parse(localStorage.getItem(RECENT_KEY) || "[]")
  } catch {
    return []
  }
}

export function EmojiImg({ e, size = 20 }: { e: EmojiEntry; size?: number }) {
  return (
    <img
      src={e.url}
      alt={e.e}
      width={size}
      height={size}
      loading="lazy"
      draggable={false}
      className="pixelated object-contain"
      style={{ width: size, height: size }}
    />
  )
}

/** The sign's pixel-art emoji (the emoji pack's pictures, or the committed emoji sheet's), as they will really look on
 * the sign. onlyNative: list only the emoji in the sign's emoji sheet (firmware with native emoji). */
export function EmojiPicker({
  onPick,
  children,
  onlyNative = false,
}: {
  onPick: (e: EmojiEntry) => void
  children: ReactNode
  onlyNative?: boolean
}) {
  const { data: catalog } = useEmoji()
  const all = useMemo(() => (onlyNative ? catalog?.filter((e) => e.native) : catalog), [catalog, onlyNative])
  const [open, setOpen] = useState(false)
  const [query, setQuery] = useState("")
  const [recent, setRecent] = useState<string[]>(loadRecent)
  const [group, setGroup] = useState(() => (loadRecent().length ? "Recent" : "Smileys"))
  const [hi, setHi] = useState("")

  const byCps = useMemo(() => new Map((all ?? []).map((e) => [e.cps, e])), [all])
  const shown = useMemo(() => {
    if (!all) return []
    const q = query.trim().toLowerCase()
    if (q) {
      const words = q.split(/\s+/)
      return all
        .filter((e) => {
          const hay = `${e.name} ${e.cps.toLowerCase()} u+${e.cps.toLowerCase()} ${e.e}`
          return words.every((w) => hay.includes(w.replace(/^u\+/, "")))
        })
        .slice(0, 240)
    }
    if (group === "Recent") return recent.map((c) => byCps.get(c)).filter((e): e is EmojiEntry => !!e)
    return all.filter((e) => e.group === group || (group === "Symbols" && e.group === "Other"))
  }, [all, query, group, recent, byCps])
  const current = byCps.get(hi) ?? shown[0]
  const refresh = useRefreshContent()
  const file = useRef<HTMLInputElement>(null)
  const importFile = async (f: File) => {
    const fd = new FormData()
    fd.append("file", f)
    fd.append("name", f.name.replace(/\.[^.]+$/, "").replace(/[-_]+/g, " "))
    try {
      const r = await api.importEmoji(fd)
      if (!r.ok) throw new Error(r.error ?? "import failed")
      await refresh()
      toast.success(`Added ${r.name} (U+${r.cps})`)
    } catch (err) {
      toast.error(`Could not import ${f.name}: ${err instanceof Error ? err.message : err}`)
    }
  }
  const remove = async (e: EmojiEntry) => {
    await api.removeEmoji(e.cps)
    await refresh()
    toast.success(`Removed ${e.name}`)
  }

  const pick = (e: EmojiEntry) => {
    const r = [e.cps, ...recent.filter((c) => c !== e.cps)].slice(0, 32)
    setRecent(r)
    try {
      localStorage.setItem(RECENT_KEY, JSON.stringify(r))
    } catch {
      /* ignore */
    }
    onPick(e)
    setOpen(false)
  }

  return (
    <Popover open={open} onOpenChange={setOpen}>
      <PopoverTrigger asChild>{children}</PopoverTrigger>
      <PopoverContent align="end" className="w-[22rem] p-0">
        <Command shouldFilter={false} value={hi} onValueChange={setHi} loop>
          <CommandInput placeholder="Search emoji" value={query} onValueChange={setQuery} />
          {!query && (
            // Ghost (variant "default") on purpose: a compact icon tab strip inside the popover.
            <ToggleGroup
              type="single"
              variant="default"
              value={group}
              onValueChange={(v) => v && setGroup(v)}
              aria-label="Emoji group"
              size="sm"
              className="w-full justify-between border-b px-1.5 py-1"
            >
              {GROUPS.map(([g, Icon]) => (
                <ToggleGroupItem key={g} value={g} aria-label={g} className="size-7 min-w-7 px-0">
                  <Tooltip>
                    <TooltipTrigger asChild>
                      <span className="grid size-full place-items-center">
                        <Icon className="size-3.5" />
                      </span>
                    </TooltipTrigger>
                    <TooltipContent>{g}</TooltipContent>
                  </Tooltip>
                </ToggleGroupItem>
              ))}
            </ToggleGroup>
          )}
          <CommandList className="h-64 max-h-64">
            {!all ? (
              <div className="grid grid-cols-8 gap-1 p-2">
                {Array.from({ length: 32 }, (_, i) => (
                  <Skeleton key={i} className="size-8" />
                ))}
              </div>
            ) : (
              <>
                <CommandEmpty>
                  {group === "Recent" && !query
                    ? "Nothing picked yet."
                    : group === "Custom" && !query
                      ? "Import a GIF or picture: it becomes an emoji of the sign, animated if the GIF is."
                      : "No emoji found."}
                </CommandEmpty>
                {shown.length > 0 && (
                  <CommandGroup
                    heading={query ? `${shown.length === 240 ? "First 240" : shown.length} matches` : group}
                    className="[&_[cmdk-group-items]]:grid [&_[cmdk-group-items]]:grid-cols-8 [&_[cmdk-group-items]]:gap-0.5"
                  >
                    {shown.map((e) => (
                      <CommandItem
                        key={e.cps}
                        value={e.cps}
                        onSelect={() => pick(e)}
                        className="size-9 justify-center p-0"
                        aria-label={e.name}
                      >
                        <EmojiImg e={e} size={32} />
                      </CommandItem>
                    ))}
                  </CommandGroup>
                )}
              </>
            )}
          </CommandList>
          <div className="flex h-11 items-center gap-2.5 border-t px-3 text-xs text-muted-foreground">
            {current ? (
              <>
                <EmojiImg e={current} size={32} />
                <span className="truncate capitalize text-foreground">{current.name}</span>
                <span className="ml-auto shrink-0 font-mono text-[10px]">U+{current.cps.split(" ")[0]}</span>
              </>
            ) : (
              <span>Pixel-art emoji</span>
            )}
            {group === "Custom" && !query && (
              <span className="ml-auto flex shrink-0 gap-1">
                {current?.custom && (
                  <Tooltip>
                    <TooltipTrigger asChild>
                      <Button size="icon" variant="ghost" className="size-7" aria-label={`Remove ${current.name}`} onClick={() => remove(current)}>
                        <Trash2 className="size-3.5" />
                      </Button>
                    </TooltipTrigger>
                    <TooltipContent>Remove this emoji</TooltipContent>
                  </Tooltip>
                )}
                <Button size="sm" variant="outline" className="h-7" onClick={() => file.current?.click()}>
                  <ImagePlus className="size-3.5" /> Import GIF
                </Button>
                <Input
                  ref={file}
                  type="file"
                  accept="image/gif,image/png,image/webp"
                  className="hidden"
                  onChange={(ev) => {
                    const f = ev.target.files?.[0]
                    ev.target.value = ""
                    if (f) void importFile(f)
                  }}
                />
              </span>
            )}
          </div>
        </Command>
      </PopoverContent>
    </Popover>
  )
}
