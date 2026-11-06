import type { ReactNode } from "react"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import { Info } from "@marquee/ui/lib/icons"

/** A titled block: short label, optional detail behind an info tooltip. */
export function Section({
  title,
  hint,
  action,
  children,
}: {
  title: string
  hint?: string
  action?: ReactNode
  children: ReactNode
}) {
  return (
    <section className="space-y-3">
      <div className="flex min-h-6 items-center gap-1.5">
        <h3 className="text-sm font-medium text-foreground/90">{title}</h3>
        {hint && (
          <Tooltip>
            <TooltipTrigger className="text-muted-foreground hover:text-foreground" aria-label={`About ${title}`}>
              <Info className="size-3.5" />
            </TooltipTrigger>
            <TooltipContent className="max-w-xs">{hint}</TooltipContent>
          </Tooltip>
        )}
        {action && <div className="ml-auto">{action}</div>}
      </div>
      {children}
    </section>
  )
}
