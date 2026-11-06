import { NavLink } from "react-router-dom"
import { SidebarMenu, SidebarMenuButton, SidebarMenuItem } from "@marquee/ui/components/ui/sidebar"
import { RectangleHorizontal } from "@marquee/ui/lib/icons"
import { useConfig } from "@/lib/api"

export function PanelBadge() {
  const { data } = useConfig()
  const p = data?.panel
  const label = p ? `${p.width} × ${p.height}${p.model ? ` · ${p.model}` : ""}` : "…"
  return (
    <SidebarMenu>
      <SidebarMenuItem>
        <SidebarMenuButton asChild size="sm" tooltip={`Panel ${label}`} className="text-muted-foreground">
          <NavLink to="/settings">
            <RectangleHorizontal />
            <span className="tabular-nums">{label}</span>
          </NavLink>
        </SidebarMenuButton>
      </SidebarMenuItem>
    </SidebarMenu>
  )
}
