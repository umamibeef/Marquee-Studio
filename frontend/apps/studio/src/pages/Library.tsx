import { useState } from "react"
import { useNavigate, useParams } from "react-router-dom"
import { SignPlayer, SignStage } from "@marquee/sign-player"
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@marquee/ui/components/ui/tabs"
import { Button } from "@marquee/ui/components/ui/button"
import { Badge } from "@marquee/ui/components/ui/badge"
import { Card } from "@marquee/ui/components/ui/card"
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from "@marquee/ui/components/ui/dialog"
import { HoverCard, HoverCardContent, HoverCardTrigger } from "@marquee/ui/components/ui/hover-card"
import { Empty, EmptyDescription, EmptyHeader, EmptyMedia, EmptyTitle } from "@marquee/ui/components/ui/empty"
import { Check, Film, Plus, Type, Upload } from "@marquee/ui/lib/icons"
import { toast } from "@marquee/ui/lib/toast"
import { useAssets, useConfig, useRefreshContent, usePanel } from "@/lib/api"
import { useRendered } from "@/lib/emu"
import type { Spec } from "@marquee/sign-emu"
import { addAnimationFiles, addFontFiles } from "@/lib/imports"
import { useLook } from "@/lib/hooks"
import { useComposer } from "@/state/composer"
import { DropZone } from "@/components/DropZone"
import { Thumb, useActive, useSeen } from "@/components/Thumb"
import { ImportAnimationDialog, ImportFontDialog } from "@/components/ImportDialogs"
import { FontSample, fontTitle, originLabel, useSampleRows } from "@/components/FontPicker"
import { animLabel } from "@/components/AnimationPicker"

/** The animation as the sign shows it behind text: tiled across the whole panel (effect slots tile). */
function useAnim(name: string, enabled: boolean) {
  const { W, H } = usePanel()
  return useRendered(["fx", name, "bg", "", "fonte04.png"],
    { text: "", font: "fonte04.png", mode: "static", background: name, colour: "Amber" } as Spec,
    W, H, enabled)
}

function AnimCard({ name, onOpen }: { name: string; onOpen: () => void }) {
  const { data: assets } = useAssets()
  const [ref, seen] = useSeen<HTMLDivElement>()
  const [active, hover] = useActive()
  const q = useAnim(name, seen)
  const info = assets?.animation_info[name]
  return (
    <div ref={ref}>
      <Button
        variant="outline"
        aria-label={`${animLabel(assets, name)}, open`}
        onClick={onOpen}
        className="h-auto w-full flex-col items-stretch gap-0 overflow-hidden rounded-xl p-0 text-left font-normal whitespace-normal"
        {...hover}
      >
        <Thumb frames={q.data} active={active} className="rounded-none ring-0" label={name} height={88} />
        <div className="flex items-center justify-between gap-2 px-3 py-2.5">
          <div className="grid min-w-0">
            <span className="truncate text-sm font-medium">{animLabel(assets, name)}</span>
            <span className="truncate text-xs text-muted-foreground">{name}</span>
          </div>
          {info?.slot != null ? (
            <Badge variant="secondary" className="shrink-0 font-normal tabular-nums">
              slot {info.slot & 0x3fff}
            </Badge>
          ) : (
            <Badge variant="outline" className="shrink-0 font-normal text-muted-foreground">
              extra
            </Badge>
          )}
        </div>
      </Button>
    </div>
  )
}

function AnimDialog({ name, onClose }: { name: string | null; onClose: () => void }) {
  const { data: assets } = useAssets()
  const { c, set } = useComposer()
  const nav = useNavigate()
  const look = useLook()
  const q = useAnim(name ?? "", !!name)
  const info = name ? assets?.animation_info[name] : undefined
  const use = (patch: Parameters<typeof set>[0], what: string) => {
    set(patch)
    toast.success(`${animLabel(assets, name!)} ${what}`, { action: { label: "Compose", onClick: () => nav("/") } })
    onClose()
  }
  return (
    <Dialog open={!!name} onOpenChange={(o) => !o && onClose()}>
      <DialogContent className="sm:max-w-2xl">
        <DialogHeader>
          <DialogTitle>{name && animLabel(assets, name)}</DialogTitle>
          <DialogDescription className="flex flex-wrap gap-x-3 gap-y-1">
            <span>{name}</span>
            {info?.w && (
              <span className="tabular-nums">
                {info.w} × {info.h} · {info.frames} frames · {info.depth}-bit · {info.sign_ms} ms a frame on the sign
              </span>
            )}
          </DialogDescription>
        </DialogHeader>
        <SignStage className="grid place-items-center rounded-xl px-6 py-8">
          <SignPlayer frames={q.data} look={{ brightness: look.brightness, gamma: look.gamma, persist: look.persist, dots: look.dots }} className="flex justify-center" />
        </SignStage>
        <DialogFooter className="sm:justify-between">
          <span className="self-center text-xs text-muted-foreground">
            {info?.slot != null ? `Effect slot ${info.slot & 0x3fff} on the sign` : "Not an effect slot: the studio previews it in a free slot"}
          </span>
          <div className="flex gap-2">
            <Button variant="outline" onClick={() => use({ background: name! }, "is the background")} disabled={c.background === name}>
              {c.background === name && <Check />}
              Use as background
            </Button>
            <Button onClick={() => use({ fill: name! }, "fills the letters")} disabled={c.fill === name}>
              {c.fill === name && <Check />}
              Fill the letters
            </Button>
          </div>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}

function Animations() {
  const { data: assets } = useAssets()
  const refresh = useRefreshContent()
  const [open, setOpen] = useState<string | null>(null)
  const anims = assets?.animations ?? []
  const drop = async (files: File[]) => {
    if ((await addAnimationFiles(files)).length) await refresh()
  }
  return (
    <DropZone onFiles={drop} label="Drop to add to the animations folder">
      <div className="mb-5 flex items-center justify-between gap-4">
        <p className="text-sm text-muted-foreground">
          {anims.length} in <Badge variant="outline" className="font-mono font-normal">studio/content/animations</Badge>
        </p>
        <ImportAnimationDialog>
          <Button size="sm">
            <Upload />
            Add animation
          </Button>
        </ImportAnimationDialog>
      </div>
      {anims.length ? (
        <div className="grid grid-cols-2 gap-4 md:grid-cols-3 xl:grid-cols-4">
          {anims.map((a) => (
            <AnimCard key={a} name={a} onOpen={() => setOpen(a)} />
          ))}
        </div>
      ) : (
        <Empty className="border border-dashed">
          <EmptyHeader>
            <EmptyMedia variant="icon">
              <Film />
            </EmptyMedia>
            <EmptyTitle>No animations yet</EmptyTitle>
            <EmptyDescription>Drop .flc, GIF, APNG or WebP files here.</EmptyDescription>
          </EmptyHeader>
        </Empty>
      )}
      <AnimDialog name={open} onClose={() => setOpen(null)} />
    </DropZone>
  )
}

function FontCard({ name }: { name: string }) {
  const { data: assets } = useAssets()
  const { c, set } = useComposer()
  const i = assets?.font_info[name]
  const sample = "ABCdef"
  const selected = c.font === name
  const rows = useSampleRows()
  return (
    <Card className="gap-0 overflow-hidden p-0">
      <SignStage className="rounded-none px-4 py-5 ring-0">
        <FontSample font={name} text={sample} rows={rows} />
      </SignStage>
      <div className="flex items-center gap-3 px-3 py-2.5">
        <div className="grid min-w-0 flex-1">
          <span className="truncate text-sm font-medium">{fontTitle(name, i)}</span>
          <span className="text-xs text-muted-foreground tabular-nums">
            {/* each piece stays whole; a narrow card wraps between them */}
            <span className="whitespace-nowrap">{name}</span>
            {i?.cell && <> · <span className="whitespace-nowrap">{i.cell[0]} × {i.cell[1]}</span></>}
          </span>
        </div>
        <HoverCard openDelay={200}>
          <HoverCardTrigger asChild>
            <Badge variant={i?.origin === "sign" ? "outline" : "secondary"} className="shrink-0 cursor-default font-normal" tabIndex={0}>
              {originLabel(i)}
            </Badge>
          </HoverCardTrigger>
          <HoverCardContent className="w-80 text-sm">
            {i?.origin === "sign" ? (
              <p>The sign's own font, extracted from its flash. For this sign only; never shared.</p>
            ) : (
              <div className="grid gap-2">
                <p className="font-medium">{i?.shareable ? "Shareable, with this credit" : "Local use only"}</p>
                {(i?.credits ?? []).map((cr) => (
                  <p key={cr} className="text-xs text-muted-foreground">
                    {cr}
                  </p>
                ))}
              </div>
            )}
          </HoverCardContent>
        </HoverCard>
        <Button size="sm" variant={selected ? "secondary" : "ghost"} onClick={() => set({ font: name })} disabled={selected}>
          {selected ? <Check /> : null}
          {selected ? "In use" : "Use"}
        </Button>
      </div>
    </Card>
  )
}

function Fonts() {
  const { data: assets } = useAssets()
  const refresh = useRefreshContent()
  const fonts = assets?.fonts ?? []
  const drop = async (files: File[]) => {
    if ((await addFontFiles(files)).length) await refresh()
  }
  return (
    <DropZone onFiles={drop} label="Drop a font PNG to add it to the fonts folder">
      <div className="mb-5 flex flex-wrap items-center justify-between gap-4">
        <p className="text-sm text-muted-foreground">
          {fonts.length} in <Badge variant="outline" className="font-mono font-normal">studio/content/fonts</Badge>
        </p>
        <div className="flex gap-2">
          <ImportFontDialog>
            <Button size="sm">
              <Plus />
              Import a free pixel font
            </Button>
          </ImportFontDialog>
        </div>
      </div>
      {fonts.length ? (
        <div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
          {fonts.map((f) => (
            <FontCard key={f} name={f} />
          ))}
        </div>
      ) : (
        <Empty className="border border-dashed">
          <EmptyHeader>
            <EmptyMedia variant="icon">
              <Type />
            </EmptyMedia>
            <EmptyTitle>No fonts yet</EmptyTitle>
            <EmptyDescription>Drop a sign font PNG here, or import a free pixel font.</EmptyDescription>
          </EmptyHeader>
        </Empty>
      )}
    </DropZone>
  )
}

export function LibraryPage() {
  const { tab = "animations" } = useParams()
  const nav = useNavigate()
  const { data: cfg } = useConfig()
  return (
    <div className="mx-auto w-full max-w-6xl px-4 py-6 sm:px-6 lg:px-8">
      <Tabs value={tab} onValueChange={(v) => nav(`/library/${v}`)} className="gap-6">
        <div className="flex items-center justify-between">
          <TabsList className="h-10">
            <TabsTrigger value="animations" className="px-4">
              <Film />
              Animations
            </TabsTrigger>
            <TabsTrigger value="fonts" className="px-4">
              <Type />
              Fonts
            </TabsTrigger>
          </TabsList>
          {cfg && <span className="text-xs text-muted-foreground tabular-nums">Previews at {cfg.panel.width} × {cfg.panel.height}</span>}
        </div>
        <TabsContent value="animations">
          <Animations />
        </TabsContent>
        <TabsContent value="fonts">
          <Fonts />
        </TabsContent>
      </Tabs>
    </div>
  )
}
