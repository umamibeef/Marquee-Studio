import { Button } from "@marquee/ui/components/ui/button"
import { Tooltip, TooltipContent, TooltipTrigger } from "@marquee/ui/components/ui/tooltip"
import { Moon, Sun } from "@marquee/ui/lib/icons"
import { useTheme } from "@marquee/ui/lib/theme"

export function ThemeToggle() {
  const { resolvedTheme, setTheme } = useTheme()
  const dark = resolvedTheme !== "light"
  return (
    <Tooltip>
      <TooltipTrigger asChild>
        <Button variant="ghost" size="icon-sm" aria-label="Toggle theme" onClick={() => setTheme(dark ? "light" : "dark")}>
          {dark ? <Sun /> : <Moon />}
        </Button>
      </TooltipTrigger>
      <TooltipContent>{dark ? "Light theme" : "Dark theme"}</TooltipContent>
    </Tooltip>
  )
}
