import { useCallback, useEffect, useRef, useState, type ReactNode } from "react"
import { SignPlayer, SignStage, type Frames, type SignPlayerHandle } from "@marquee/sign-player"
import { Card } from "@marquee/ui/components/ui/card"
import { Button } from "@marquee/ui/components/ui/button"
import { Slider } from "@marquee/ui/components/ui/slider"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import { Loader2, Pause, Play } from "@marquee/ui/lib/icons"
import { cn } from "@marquee/ui/lib/utils"
import { useLook } from "@/lib/hooks"

/** The hero: the sign itself, large, on a dark stage, with play / seek and a slot for actions. */
export function Stage({
  frames,
  busy,
  actions,
  footer,
  className,
  playerRef,
}: {
  frames: Frames | null | undefined
  busy?: boolean
  actions?: ReactNode
  footer?: ReactNode
  className?: string
  playerRef?: React.RefObject<SignPlayerHandle | null>
}) {
  const own = useRef<SignPlayerHandle>(null)
  const ref = playerRef ?? own
  const [playing, setPlaying] = useState(true)
  const [frame, setFrame] = useState(0)
  const look = useLook()
  const n = frames?.nframes ?? 0
  const ms = frames?.ms ?? 25
  const onFrame = useCallback((i: number) => setFrame(i), [])

  useEffect(() => {
    setFrame((f) => (n ? f % n : 0))
  }, [n])

  const secs = (x: number) => `${((x * ms) / 1000).toFixed(1)} s`
  return (
    <Card className={cn("gap-0 rounded-2xl p-3", className)}>
      <SignStage className="grid place-items-center rounded-xl px-6 py-6 sm:px-8 sm:py-7">
        <div
          aria-hidden
          className="pointer-events-none absolute inset-0 opacity-60"
          style={{ background: "radial-gradient(60% 80% at 50% 50%, rgba(255,255,255,0.035), transparent 70%)" }}
        />
        <SignPlayer
          ref={ref}
          frames={frames}
          playing={playing}
          keepPosition
          onFrame={onFrame}
          look={{ brightness: look.brightness, gamma: look.gamma, persist: look.persist, dots: look.dots }}
          className="relative flex justify-center"
        />
        {busy && (
          <Loader2 className="absolute top-3 right-3 size-4 animate-spin text-white/40" aria-label="Rendering" />
        )}
      </SignStage>
      <div className="flex flex-wrap items-center gap-3 px-1 pt-3">
        <Tooltip>
          <TooltipTrigger asChild>
            <Button
              variant="ghost"
              size="icon-sm"
              aria-label={playing ? "Pause" : "Play"}
              onClick={() => setPlaying((p) => !p)}
              disabled={n <= 1}
            >
              {playing ? <Pause /> : <Play />}
            </Button>
          </TooltipTrigger>
          <TooltipContent>{playing ? "Pause" : "Play"}</TooltipContent>
        </Tooltip>
        <Slider
          className="min-w-32 flex-1"
          aria-label="Seek"
          min={0}
          max={Math.max(0, n - 1)}
          step={1}
          value={[frame]}
          disabled={n <= 1}
          onValueChange={([v]) => {
            setPlaying(false)
            ref.current?.seek(v)
            setFrame(v)
          }}
        />
        <span className="w-24 text-right text-xs text-muted-foreground tabular-nums">
          {n > 1 ? `${secs(frame)} / ${secs(n)}` : n === 1 ? "still" : ""}
        </span>
        {actions && <div className="flex items-center gap-2">{actions}</div>}
      </div>
      {footer}
    </Card>
  )
}
