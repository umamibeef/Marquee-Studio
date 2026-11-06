import type { HTMLAttributes } from "react"
import { cn } from "@marquee/ui/lib/utils"

/** The dark LED stage a <SignPlayer> sits on: part of the sign preview exception (docs/PORTAL-PLAN.md §7), the
 * same in light and dark themes (tokens --stage / --stage-ring from @marquee/ui). Size and padding come from the
 * caller's className. */
export function SignStage({ className, ...props }: HTMLAttributes<HTMLDivElement>) {
  return <div {...props} className={cn("relative overflow-hidden rounded-md bg-stage ring-1 ring-stage-ring", className)} />
}
