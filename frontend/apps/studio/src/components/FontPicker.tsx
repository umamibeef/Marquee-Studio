import { useQuery, useQueryClient } from "@tanstack/react-query"
import { ToggleGroup, ToggleGroupItem } from "@marquee/ui/components/ui/toggle-group"
import { Badge } from "@marquee/ui/components/ui/badge"
import { SignPlayer, SignStage } from "@marquee/sign-player"
import { Skeleton } from "@marquee/ui/components/ui/skeleton"
import { useAssets, type FontInfo } from "@/lib/api"
import { fontSample } from "@/lib/emu"
import { useSeen } from "./Thumb"

export function originLabel(i: FontInfo | undefined) {
  if (!i) return ""
  if (i.origin === "sign") return "Sign's own"
  return i.licence ?? "Imported"
}

export function fontTitle(name: string, i: FontInfo | undefined) {
  const n = name.replace(/\.png$/i, "")
  if (i?.origin === "import" && i.source) return i.source.split("/").pop()!.replace(/\.(bdf|ttf)$/i, "")
  return n
}

/** The tallest sign font's sample height in LEDs: every sample is drawn at one LED size (height / this), so the
 *  fonts keep their true sizes next to each other instead of each being stretched to the same height. */
export function useSampleRows() {
  const { data: assets } = useAssets()
  return Math.max(16, ...(assets?.fonts ?? []).map((f) => (assets?.font_info[f]?.cell?.[1] ?? 0) + 2))
}

/** Device pixels per LED in font samples: the smallest pitch that draws round LEDs. */
const SAMPLE_PITCH = 4

/** A font's sample text as the sign draws it (emulated here), every font at the same LED size (round LEDs, a whole
 *  number of device pixels each): the box is `rows` LEDs tall, the tallest font's sample. */
export function FontSample({ font, text, rows }: { font: string; text: string; rows: number }) {
  const css = SAMPLE_PITCH / (window.devicePixelRatio || 1)
  const height = Math.ceil(rows * css)
  const [ref, seen] = useSeen<HTMLDivElement>()
  const qc = useQueryClient()
  const q = useQuery({
    queryKey: ["font-sample", font, text],
    queryFn: () => fontSample(qc, font, text),
    enabled: seen,
    staleTime: Infinity,
  })
  return (
    <div ref={ref} className="flex w-full items-center justify-start overflow-hidden" style={{ height }}>
      {q.data ? (
        <SignPlayer
          frames={q.data}
          playing={false}
          glow={false}
          maxWidth={q.data.w * css}
          look={{ dots: true, unlit: false, persist: 0 }}
          label={`${font} sample`}
        />
      ) : (
        <Skeleton className="h-4 w-24 bg-white/5" />
      )}
    </div>
  )
}

/** Every font in the content folder, each shown in itself. */
export function FontPicker({ value, onChange, sample }: { value: string; onChange: (v: string) => void; sample: string }) {
  const { data: assets } = useAssets()
  const rows = useSampleRows()
  const fonts = assets?.fonts ?? []
  return (
    <ToggleGroup
      type="single"
      variant="outline"
      value={value}
      onValueChange={(v) => v && onChange(v)}
      spacing={2}
      aria-label="Font"
      className="grid w-full grid-cols-1 gap-2 md:grid-cols-2 xl:grid-cols-3"
    >
      {fonts.map((f) => {
        const i = assets?.font_info[f]
        return (
          <ToggleGroupItem
            key={f}
            value={f}
            aria-label={f}
            className="flex h-auto w-full flex-col items-stretch gap-2 rounded-lg p-2 data-[state=on]:border-foreground/60 data-[state=on]:bg-accent/60"
          >
            <SignStage className="px-2 py-2.5">
              <FontSample font={f} text={sample} rows={rows} />
            </SignStage>
            <span className="flex items-center justify-between gap-2 px-0.5 text-xs">
              <span className="truncate">{fontTitle(f, i)}</span>
              <span className="flex shrink-0 items-center gap-1 text-muted-foreground">
                {i?.cell && <span className="tabular-nums">{i.cell[1]}px</span>}
                {i?.origin !== "sign" && (
                  <Badge variant="secondary" className="h-4 px-1 text-[10px] font-normal">
                    free
                  </Badge>
                )}
              </span>
            </span>
          </ToggleGroupItem>
        )
      })}
    </ToggleGroup>
  )
}
