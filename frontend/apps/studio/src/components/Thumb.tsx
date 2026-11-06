import { useEffect, useRef, useState } from "react"
import { SignPlayer, SignStage, type Frames } from "@marquee/sign-player"
import { Skeleton } from "@marquee/ui/components/ui/skeleton"
import { cn } from "@marquee/ui/lib/utils"
import { useLook } from "@/lib/hooks"

/** True once the element has been on screen (lazy thumbnails). */
export function useSeen<T extends Element>() {
  const ref = useRef<T>(null)
  const [seen, setSeen] = useState(false)
  useEffect(() => {
    const el = ref.current
    if (!el || seen) return
    const io = new IntersectionObserver((es) => es.some((e) => e.isIntersecting) && setSeen(true), {
      rootMargin: "200px",
    })
    io.observe(el)
    return () => io.disconnect()
  }, [seen])
  return [ref, seen] as const
}

/** A small LED preview that plays while `active` (hovered or focused), else holds its first frame. */
export function Thumb({
  frames,
  active,
  className,
  label,
  height,
}: {
  frames: Frames | null | undefined
  active: boolean
  className?: string
  label?: string
  /** fixed CSS height of the preview area; the panel keeps its aspect inside it */
  height?: number
}) {
  const look = useLook()
  return (
    <SignStage
      className={cn("flex w-full items-center", className)}
      style={height ? { height } : undefined}
    >
      {frames ? (
        <div className="flex w-full min-w-0 justify-center p-1.5">
          <SignPlayer
            frames={frames}
            maxWidth={height ? ((height - 12) * frames.w) / frames.h : undefined}
            className="flex justify-center"
            playing={active}
            glow={false}
            label={label}
            look={{ brightness: look.brightness, gamma: look.gamma, persist: look.persist, dots: true, unlit: false }}
          />
        </div>
      ) : (
        <Skeleton className={cn("w-full rounded-none bg-white/5", !height && "aspect-[5/1]")} style={height ? { height } : undefined} />
      )}
    </SignStage>
  )
}

/** Hover / focus state for a thumbnail's container. */
export function useActive() {
  const [active, setActive] = useState(false)
  const props = {
    onMouseEnter: () => setActive(true),
    onMouseLeave: () => setActive(false),
    onFocus: () => setActive(true),
    onBlur: () => setActive(false),
  }
  return [active, props] as const
}
