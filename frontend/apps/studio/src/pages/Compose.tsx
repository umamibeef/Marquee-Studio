import { useEffect, useMemo, useRef, useState } from "react"
import { keepPreviousData, useQuery, useQueryClient } from "@tanstack/react-query"
import type { SignPlayerHandle } from "@marquee/sign-player"
import { Button } from "@marquee/ui/components/ui/button"
import { Textarea } from "@marquee/ui/components/ui/textarea"
import { Label } from "@marquee/ui/components/ui/label"
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@marquee/ui/components/ui/tabs"
import { Alert, AlertDescription, AlertTitle } from "@marquee/ui/components/ui/alert"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import { Braces, Copy, Download, Loader2, MonitorUp, Send, Smile, TriangleAlert } from "@marquee/ui/lib/icons"
import { toast } from "@marquee/ui/lib/toast"
import { downloadBlob, useAssets, useConfig, useEmoji, type EmojiEntry, usePanel } from "@/lib/api"
import { emojiIn, stripVs, useDebounced } from "@/lib/hooks"
import { useImportGif } from "@/lib/useImportGif"
import { charFx, fromStyleTags, NO_FX, realignFx, toSpec, toStyleTags, useComposer, withEffects, type CharFx } from "@/state/composer"
import { contentBytes, renderImgs, toFrames, useSign } from "@/lib/emu"
import { encodeGif, flashWarnings, renderMessage, type Spec } from "@marquee/sign-emu"
import { Stage } from "@/components/Stage"
import { Presets } from "@/components/Presets"
import { EmojiImg, EmojiPicker } from "@/components/EmojiPicker"
import { SendDialog } from "@/components/SendDialog"
import { LineDialog } from "@/components/LineDialog"
import { MotionPicker } from "@/components/MotionPicker"
import { MotionBackdrop } from "@/components/CharMotion"
import { FormatPalette } from "@/components/FormatPalette"

/** Emoji in the text that the sign cannot draw: with native emoji, those outside the sign's emoji sheet; with the
 * stock firmware (no native emoji), every emoji. */
function useMissingEmoji(text: string, native: boolean) {
  const { data: catalog } = useEmoji()
  return useMemo(() => {
    const onSign = new Set(native ? (catalog ?? []).filter((e) => e.native).map((e) => stripVs(e.e)) : [])
    const known = new Map((catalog ?? []).map((e) => [stripVs(e.e), e]))
    const seen = new Set<string>()
    const out: { e: string; entry?: EmojiEntry }[] = []
    for (const e of emojiIn(text)) {
      const k = stripVs(e)
      if (onSign.has(k) || seen.has(k)) continue
      seen.add(k)
      out.push({ e, entry: known.get(k) })
    }
    return out
  }, [catalog, text, native])
}

export function ComposePage() {
  const { c, set, undo, redo } = useComposer()
  const { data: assets } = useAssets()
  const { data: cfg } = useConfig()
  const player = useRef<SignPlayerHandle>(null)
  const text = useRef<HTMLTextAreaElement>(null)
  const [gifBusy, setGifBusy] = useState(false)

  const spec = useMemo(() => toSpec(c, assets?.palette), [c, assets])
  const debounced = useDebounced(spec, 220)
  // the preview is the sign emulated here (@marquee/sign-emu renderMessage, checked frame for frame against the
  // Python renderer): the server only hands over the fonts, the animations and the emoji sheet
  const sign = useSign()
  const qc = useQueryClient()
  const { W, H } = usePanel()
  const nonSlot = [debounced.foreground, debounced.background].filter(
    (n): n is string => typeof n === "string" && n.endsWith(".flc") && assets?.animation_info?.[n]?.slot == null)
  const anims = useQuery({
    queryKey: ["anim-bytes", ...nonSlot],
    queryFn: async () => new Map(await Promise.all(nonSlot.map(async (n) => [n, await contentBytes("animations", n)] as const))),
    placeholderData: keepPreviousData,
    staleTime: Infinity,
  })
  const preview = useMemo(() => {
    if (!sign.data || !cfg || !anims.data) return { data: undefined, isFetching: true }
    const nat = !!cfg.native_emoji && !!sign.data.sheet
    try {
      const frames = renderMessage({ ...debounced, native_emoji: nat, emoji_size: sign.data.sheet?.size ?? H } as Spec, W, H,
        { fonts: new Map(sign.data.fonts.map((f, i) => [sign.data!.fontNames[i], f])), slots: sign.data.slots, anims: anims.data, sheet: sign.data.sheet })
      const warnings = flashWarnings(Number(debounced.flash_on_ms) || 0, Number(debounced.flash_off_ms) || 0)
      return { data: { ...toFrames(frames, W, H), warnings }, isFetching: false }
    } catch (e) {
      return { data: undefined, isFetching: false, error: String(e) }
    }
  }, [sign.data, cfg, anims.data, debounced, W, H])
  const native = !!cfg?.native_emoji
  const missing = useMissingEmoji(c.text, native)

  const [sel, setSel] = useState<[number, number]>([0, 0])
  const fx = useMemo(() => charFx(c), [c])
  const readSel = () => {
    const el = text.current
    if (el) setSel([el.selectionStart, el.selectionEnd])
  }
  // Undo and redo for the whole design, in the message box too: the browser's own undo can't follow the palette's
  // edits. Other fields (the on/off times, a dialog's) keep their own.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (!(e.metaKey || e.ctrlKey) || e.altKey) return
      const k = e.key.toLowerCase()
      const isRedo = (k === "z" && e.shiftKey) || (k === "y" && !e.shiftKey)
      if (k !== "z" && !isRedo) return
      const t = e.target as HTMLElement | null
      if (t !== text.current && t?.closest("input, textarea, select, [contenteditable], [role=dialog]")) return
      e.preventDefault()
      if (isRedo) redo()
      else undo()
    }
    window.addEventListener("keydown", onKey)
    return () => window.removeEventListener("keydown", onKey)
  }, [undo, redo])
  // the box's value changes without a selection event: keep the palette's scope true to what the box shows
  useEffect(() => {
    const el = text.current
    if (el && document.activeElement === el) setSel([el.selectionStart, el.selectionEnd])
  }, [c.text])

  const [held, setHeld] = useState(false)
  // the selection, clamped to the text; the box keeps it through the palette (the box's focus comes back afterwards)
  const range: [number, number] = [Math.min(sel[0], c.text.length), Math.min(sel[1], c.text.length)]
  const restoreSel = () => {
    const [a, b] = range
    requestAnimationFrame(() => {
      text.current?.focus()
      text.current?.setSelectionRange(a, b)
    })
  }
  /** to the selected letters, or to every letter when nothing is selected */
  const applyFx = (patch: Partial<CharFx> | ((v: CharFx) => CharFx)) => {
    const [a, b] = range
    const whole = b <= a
    set({ fx: fx.map((v, i) => (whole || (i >= a && i < b) ? (typeof patch === "function" ? patch(v) : { ...v, ...patch }) : v)) })
    if (!held) restoreSel()
  }
  const BOX = "px-4 py-3.5 pr-28 text-lg leading-relaxed md:text-lg"

  const insert = (s: string) => {
    const el = text.current
    const a = el?.selectionStart ?? c.text.length
    const b = el?.selectionEnd ?? a
    const now = c.text.slice(0, a) + s + c.text.slice(b)
    set({ text: now, fx: realignFx(c.text, fx, now) })
    requestAnimationFrame(() => {
      el?.focus()
      el?.setSelectionRange(a + s.length, a + s.length)
    })
  }

  const gif = useImportGif(insert)
  const [dropping, setDropping] = useState(false)

  const copy = async () => {
    try {
      const blob = await player.current?.toBlob()
      if (blob && "ClipboardItem" in window) {
        await navigator.clipboard.write([new ClipboardItem({ "image/png": blob })])
        toast.success("Picture copied")
      } else {
        await navigator.clipboard.writeText(c.text)
        toast.success("Text copied")
      }
    } catch (e) {
      toast.error("Could not copy", { description: (e as Error).message })
    }
  }

  // The design's per-character effects as readable style tags ([wave], [fill:C002], [none], ...). Pasting them back
  // into a message reapplies the effects (fromStyleTags parses them on input).
  const copyRaw = async () => {
    const tags = toStyleTags(c.text, fx)
    try {
      await navigator.clipboard.writeText(tags)
      toast.success("Style tags copied", { description: "paste into a message to reapply the effects" })
    } catch (e) {
      toast.error("Could not copy", { description: (e as Error).message })
    }
  }

  const saveGif = async () => {
    setGifBusy(true)
    try {
      // drawn here like the preview; GIF delays are in 10 ms, so every other 25 ms frame, 50 ms each
      const frames = await renderImgs(qc, { ...spec, native_emoji: !!cfg?.native_emoji } as Spec, W, H)
      const gif = encodeGif(frames.filter((_, k) => frames.length <= 2 || k % 2 === 0), 8, frames.length > 2 ? 5 : 3)
      await downloadBlob(new Blob([gif as BlobPart], { type: "image/gif" }), "sign.gif")
    } catch (e) {
      toast.error("Could not make the GIF", { description: (e as Error).message })
    } finally {
      setGifBusy(false)
    }
  }

  const warnings = preview.data?.warnings ?? []

  return (
    <div className="mx-auto w-full max-w-6xl space-y-8 px-4 py-6 sm:px-6 lg:px-8">
      <Stage
        frames={preview.data}
        busy={preview.isFetching}
        playerRef={player}
        className="sticky top-16 z-10 bg-background/95 backdrop-blur supports-[backdrop-filter]:bg-background/80"
        actions={
          <>
            <Tooltip>
              <TooltipTrigger asChild>
                <Button variant="ghost" size="icon-sm" aria-label="Copy picture" onClick={copy}>
                  <Copy />
                </Button>
              </TooltipTrigger>
              <TooltipContent>Copy picture</TooltipContent>
            </Tooltip>
            <Tooltip>
              <TooltipTrigger asChild>
                <Button variant="ghost" size="icon-sm" aria-label="Copy style tags" onClick={copyRaw}>
                  <Braces />
                </Button>
              </TooltipTrigger>
              <TooltipContent>Copy raw bytes (escape codes)</TooltipContent>
            </Tooltip>
            <Tooltip>
              <TooltipTrigger asChild>
                <Button variant="ghost" size="icon-sm" aria-label="Save as GIF" onClick={saveGif} disabled={gifBusy}>
                  {gifBusy ? <Loader2 className="animate-spin" /> : <Download />}
                </Button>
              </TooltipTrigger>
              <TooltipContent>Save as GIF</TooltipContent>
            </Tooltip>
            <LineDialog composer={c}>
              <Button size="sm" variant="outline" className="ml-1">
                <MonitorUp />
                Show on the sign
              </Button>
            </LineDialog>
            <SendDialog payload={{ text: withEffects(c.text, fx), font: c.font }}>
              <Button size="sm" className="ml-1">
                <Send />
                Send to sign
              </Button>
            </SendDialog>
          </>
        }
        footer={
          warnings.length > 0 && (
            <Alert className="mt-3 border-amber-500/30 text-amber-600 dark:text-amber-400">
              <TriangleAlert />
              <AlertTitle>Flash is too fast</AlertTitle>
              <AlertDescription className="text-amber-700/90 dark:text-amber-300/80">{warnings.join(" ")}</AlertDescription>
            </Alert>
          )
        }
      />

      <section className="space-y-3">
        <Label htmlFor="message" className="text-base font-medium">
          What should the sign say?
        </Label>
        <FormatPalette fx={fx} sel={range} onFx={applyFx} onClear={() => applyFx(() => NO_FX)} onHold={setHeld} onRestore={restoreSel} onGif={gif.importFiles} gifBusy={gif.busy} />
        <div
          className={`relative rounded-md ${dropping ? "ring-2 ring-primary" : ""}`}
          onDragOver={(e) => {
            if (!e.dataTransfer.types.includes("Files")) return
            e.preventDefault()
            setDropping(true)
          }}
          onDragLeave={() => setDropping(false)}
          onDrop={(e) => {
            setDropping(false)
            if (!e.dataTransfer.files.length) return
            e.preventDefault()
            void gif.importFiles(e.dataTransfer.files)
          }}
        >
          <MotionBackdrop
            text={c.text}
            fx={fx}
            held={held && range[1] > range[0] ? range : null}
            className={`pointer-events-none absolute inset-0 overflow-hidden rounded-md border border-transparent whitespace-pre-wrap break-words text-transparent ${BOX}`}
          />
          <Textarea
            id="message"
            ref={text}
            onSelect={readSel}
            onKeyUp={readSel}
            onMouseUp={readSel}
            value={c.text}
            onChange={(e) => {
              const v = e.target.value
              const p = fromStyleTags(v)
              if (p.text !== v) set({ text: p.text, fx: p.fx })                        // style tags pasted/typed: apply
              else set({ text: v, fx: realignFx(c.text, fx, v) })
            }}
            placeholder="Type your message"
            spellCheck={false}
            className={`relative min-h-24 resize-none ${BOX}`}
          />
          <div className="absolute right-2 bottom-2 flex items-center gap-1">
            <EmojiPicker onPick={(e) => insert(e.e)} onlyNative={native}>
              <Button variant="ghost" size="icon-sm" aria-label="Insert emoji">
                <Smile />
              </Button>
            </EmojiPicker>
          </div>
        </div>
        {missing.length > 0 && (
          <Alert className="flex items-center gap-3 py-2.5 [&>svg]:translate-y-0">
            <Smile />
            <AlertDescription className="flex flex-1 flex-wrap items-center gap-x-2">
              <span className="flex items-center gap-1">
                {missing.map((m) => (m.entry ? <EmojiImg key={m.e} e={m.entry} size={20} /> : <span key={m.e}>{m.e}</span>))}
              </span>
              <span>
                {native
                  ? "not in the sign's emoji set; it is sent as a question mark."
                  : "needs native emoji (Settings); the stock firmware cannot draw it."}
              </span>
            </AlertDescription>
          </Alert>
        )}
      </section>

      <Tabs defaultValue="style" className="gap-6">
        <TabsList className="h-10">
          <TabsTrigger value="style" className="px-4">Style</TabsTrigger>
          <TabsTrigger value="motion" className="px-4">Movement</TabsTrigger>
        </TabsList>

        <TabsContent value="style">
          <Presets />
        </TabsContent>

        <TabsContent value="motion">
          <MotionPicker />
        </TabsContent>

      </Tabs>
    </div>
  )
}
