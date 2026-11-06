import { useMemo, useState, type ReactNode } from "react"
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
import { Input } from "@marquee/ui/components/ui/input"
import { Label } from "@marquee/ui/components/ui/label"
import { Badge } from "@marquee/ui/components/ui/badge"
import { RadioGroup, RadioGroupItem } from "@marquee/ui/components/ui/radio-group"
import {
  Select,
  SelectContent,
  SelectGroup,
  SelectItem,
  SelectLabel,
  SelectTrigger,
  SelectValue,
} from "@marquee/ui/components/ui/select"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import { Info, Loader2 } from "@marquee/ui/lib/icons"
import { toast } from "@marquee/ui/lib/toast"
import { api, useAssets, useRefreshContent } from "@/lib/api"
import { addAnimationFiles } from "@/lib/imports"
import { useComposer } from "@/state/composer"

function Field({ label, hint, htmlFor, children }: { label: string; hint?: string; htmlFor?: string; children: ReactNode }) {
  return (
    <div className="grid gap-2">
      <div className="flex items-center gap-1.5">
        <Label htmlFor={htmlFor}>{label}</Label>
        {hint && (
          <Tooltip>
            <TooltipTrigger className="text-muted-foreground" aria-label={`About ${label}`}>
              <Info className="size-3.5" />
            </TooltipTrigger>
            <TooltipContent className="max-w-xs">{hint}</TooltipContent>
          </Tooltip>
        )}
      </div>
      {children}
    </div>
  )
}

/** GIF / APNG / WebP (converted) or .flc (as is) into the animations folder. */
export function ImportAnimationDialog({ children }: { children: ReactNode }) {
  const [open, setOpen] = useState(false)
  const [file, setFile] = useState<File | null>(null)
  const [name, setName] = useState("")
  const [fit, setFit] = useState("contain")
  const [depth, setDepth] = useState("16")
  const [busy, setBusy] = useState(false)
  const refresh = useRefreshContent()
  const isFlc = !!file && /\.flc$/i.test(file.name)
  const go = async () => {
    if (!file) return
    setBusy(true)
    const added = await addAnimationFiles([file], { name: name || file.name, fit, depth })
    setBusy(false)
    if (added.length) {
      await refresh()
      setOpen(false)
      setFile(null)
      setName("")
    }
  }
  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>{children}</DialogTrigger>
      <DialogContent className="sm:max-w-md">
        <DialogHeader>
          <DialogTitle>Add an animation</DialogTitle>
          <DialogDescription>GIF, animated PNG or WebP, sized to the panel. Or an .flc as it is.</DialogDescription>
        </DialogHeader>
        <div className="grid gap-4">
          <Field label="File" htmlFor="anim-file">
            <Input
              id="anim-file"
              type="file"
              accept=".gif,.png,.apng,.webp,.flc,image/gif,image/png,image/webp"
              onChange={(e) => {
                const f = e.target.files?.[0] ?? null
                setFile(f)
                if (f && !name) setName(f.name.replace(/\.[^.]+$/, ""))
              }}
            />
          </Field>
          <Field
            label="Name on the sign"
            htmlFor="anim-name"
            hint="effect00..effect99 fill that effect slot; attract00.., winner00.. their slots. Any other name works as a fill or background in the studio."
          >
            <Input id="anim-name" value={name} onChange={(e) => setName(e.target.value)} placeholder="e.g. effect20" />
          </Field>
          {!isFlc && (
            <div className="grid grid-cols-2 gap-4">
              <Field label="Fit">
                <Select value={fit} onValueChange={setFit}>
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="contain">Fit inside</SelectItem>
                    <SelectItem value="cover">Fill (crop)</SelectItem>
                    <SelectItem value="stretch">Stretch</SelectItem>
                  </SelectContent>
                </Select>
              </Field>
              <Field label="Colour" hint="16-bit keeps every frame's own colours; 8-bit shares one 256-colour palette (smaller).">
                <Select value={depth} onValueChange={setDepth}>
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="16">16-bit</SelectItem>
                    <SelectItem value="8">8-bit palette</SelectItem>
                  </SelectContent>
                </Select>
              </Field>
            </div>
          )}
        </div>
        <DialogFooter>
          <Button variant="ghost" onClick={() => setOpen(false)}>
            Cancel
          </Button>
          <Button onClick={go} disabled={!file || busy}>
            {busy && <Loader2 className="animate-spin" />}
            {isFlc ? "Add" : "Convert"}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}

/** A vendored, permissively licensed pixel font (third_party/fonts) as a new sign font. */
export function ImportFontDialog({ children }: { children: ReactNode }) {
  const { data: assets } = useAssets()
  const { set } = useComposer()
  const refresh = useRefreshContent()
  const [open, setOpen] = useState(false)
  const [path, setPath] = useState("spleen/spleen-8x16.bdf")
  const [upper, setUpper] = useState("latin1")
  const [busy, setBusy] = useState(false)
  const fams = useMemo(() => {
    const m = new Map<string, { path: string; licence: string }[]>()
    for (const f of assets?.importable_fonts ?? []) m.set(f.family, [...(m.get(f.family) ?? []), f])
    return [...m.entries()]
  }, [assets])
  const chosen = assets?.importable_fonts.find((f) => f.path === path)
  const go = async () => {
    setBusy(true)
    try {
      const r = await api.importFont(path, upper)
      await refresh()
      toast.success(`Imported as ${r.name.replace(".png", "")}`, {
        description: `${r.cell[0]} × ${r.cell[1]} cells · ${r.codes} characters · ${r.licence}`,
        action: { label: "Use it", onClick: () => set({ font: r.name }) },
      })
      setOpen(false)
    } catch (e) {
      toast.error("Could not import the font", { description: (e as Error).message })
    } finally {
      setBusy(false)
    }
  }
  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>{children}</DialogTrigger>
      <DialogContent className="sm:max-w-md">
        <DialogHeader>
          <DialogTitle>Import a free pixel font</DialogTitle>
          <DialogDescription>Open-licence fonts you can share, credit included.</DialogDescription>
        </DialogHeader>
        <div className="grid gap-5">
          <Field label="Font">
            <Select value={path} onValueChange={setPath}>
              <SelectTrigger className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent className="max-h-80">
                {fams.map(([fam, fs]) => (
                  <SelectGroup key={fam}>
                    <SelectLabel>{fam}</SelectLabel>
                    {fs.map((f) => (
                      <SelectItem key={f.path} value={f.path}>
                        {f.path.split("/").pop()}
                      </SelectItem>
                    ))}
                  </SelectGroup>
                ))}
              </SelectContent>
            </Select>
            {chosen && (
              <div>
                <Badge variant="secondary" className="font-normal">
                  {chosen.licence}
                </Badge>
              </div>
            )}
          </Field>
          <Field
            label="Upper codes (0xA0–0xFF)"
            hint="The sign has 256 codes per font. Latin-1 letters give accents and £; leaving them free makes room for up to 128 colour emoji later."
          >
            <RadioGroup value={upper} onValueChange={setUpper} className="grid gap-2.5">
              <div className="flex items-center gap-2.5">
                <RadioGroupItem value="latin1" id="up-latin1" />
                <Label htmlFor="up-latin1" className="font-normal">
                  Accented letters (é, ü, £ …)
                </Label>
              </div>
              <div className="flex items-center gap-2.5">
                <RadioGroupItem value="none" id="up-none" />
                <Label htmlFor="up-none" className="font-normal">
                  Keep free for emoji
                </Label>
              </div>
            </RadioGroup>
          </Field>
        </div>
        <DialogFooter>
          <Button variant="ghost" onClick={() => setOpen(false)}>
            Cancel
          </Button>
          <Button onClick={go} disabled={busy || !chosen}>
            {busy && <Loader2 className="animate-spin" />}
            Import
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
