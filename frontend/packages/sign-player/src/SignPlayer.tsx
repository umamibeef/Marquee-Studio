import { useEffect, useImperativeHandle, useRef, type Ref } from "react"
import { DEFAULT_LOOK, PanelPlayer, type Frames, type LookOptions } from "./engine"

export interface SignPlayerHandle {
  play(): void
  pause(): void
  seek(i: number): void
  toBlob(): Promise<Blob | null>
  readonly frameCount: number
}

export interface SignPlayerProps {
  frames: Frames | null | undefined
  /** play (true) or hold the current frame */
  playing?: boolean
  look?: Partial<LookOptions>
  /** a soft bloom behind the LEDs */
  glow?: boolean
  /** keep the frame position when new frames of the same size arrive (live editing) */
  keepPosition?: boolean
  /** largest CSS width; the panel fills its container up to this */
  maxWidth?: number
  className?: string
  label?: string
  onFrame?: (i: number) => void
  ref?: Ref<SignPlayerHandle>
}

/** The sign preview: RGB555 frames drawn as LEDs on a dark stage (canvas). The only custom visual component in
 * the studio; its controls are shadcn components around it. */
export function SignPlayer({
  frames,
  playing = true,
  look,
  glow = true,
  keepPosition = false,
  maxWidth,
  className,
  label = "Sign preview",
  onFrame,
  ref,
}: SignPlayerProps) {
  const box = useRef<HTMLDivElement>(null)
  const canvas = useRef<HTMLCanvasElement>(null)
  const glowCanvas = useRef<HTMLCanvasElement>(null)
  const player = useRef<PanelPlayer | null>(null)
  // The latest props, read by the long-lived player and its ResizeObserver.
  const latest = useRef({ frames, playing, look, maxWidth, keepPosition, onFrame })
  latest.current = { frames, playing, look, maxWidth, keepPosition, onFrame }
  const lookKey = JSON.stringify(look ?? {})

  const fit = () => {
    const w = box.current?.clientWidth ?? 0
    const m = latest.current.maxWidth
    player.current?.layout(m ? Math.min(m, w) : w)
  }

  useEffect(() => {
    const L = latest.current
    const p = new PanelPlayer(canvas.current!, glow ? glowCanvas.current : null, { ...DEFAULT_LOOK, ...L.look })
    player.current = p
    p.onFrame = (i) => latest.current.onFrame?.(i)
    if (L.frames) {
      p.load(L.frames)
      fit()
      if (L.playing) p.play()
    }
    const ro = new ResizeObserver(fit)
    ro.observe(box.current!)
    return () => {
      ro.disconnect()
      p.dispose()
      player.current = null
    }
     
  }, [glow])

  useEffect(fit, [maxWidth])

  useEffect(() => {
    player.current?.setLook({ ...DEFAULT_LOOK, ...latest.current.look })
  }, [lookKey])

  useEffect(() => {
    const p = player.current
    if (!p || !frames) return
    p.load(frames, latest.current.keepPosition)
    fit()
    if (latest.current.playing) p.play()
    else p.draw()
     
  }, [frames])

  useEffect(() => {
    const p = player.current
    if (!p) return
    if (playing) p.play()
    else p.pause()
  }, [playing])

  useImperativeHandle(
    ref,
    () => ({
      play: () => player.current?.play(),
      pause: () => player.current?.pause(),
      seek: (i: number) => player.current?.seek(i),
      toBlob: () => player.current?.toBlob() ?? Promise.resolve(null),
      get frameCount() {
        return player.current?.frameCount ?? 0
      },
    }),
    [],
  )

  return (
    <div ref={box} className={className} style={{ position: "relative", width: "100%", minWidth: 0, overflow: "hidden", lineHeight: 0 }}>
      <div style={{ position: "relative", display: "inline-block" }}>
        <canvas ref={canvas} role="img" aria-label={label} style={{ display: "block", imageRendering: "pixelated" }} />
        {glow && (
          <canvas
            ref={glowCanvas}
            aria-hidden
            style={{
              position: "absolute",
              inset: 0,
              pointerEvents: "none",
              mixBlendMode: "screen",
              opacity: 0.4,
            }}
          />
        )}
      </div>
    </div>
  )
}
