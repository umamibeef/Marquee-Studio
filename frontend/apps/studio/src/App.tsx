import { NavLink, Route, Routes, useLocation } from "react-router-dom"
import {
  Sidebar,
  SidebarContent,
  SidebarFooter,
  SidebarGroup,
  SidebarGroupContent,
  SidebarHeader,
  SidebarInset,
  SidebarMenu,
  SidebarMenuButton,
  SidebarMenuItem,
  SidebarProvider,
  SidebarTrigger,
} from "@marquee/ui/components/ui/sidebar"
import { Separator } from "@marquee/ui/components/ui/separator"
import { Boxes, Library, ListVideo, Package, PenLine, Settings2 } from "@marquee/ui/lib/icons"
import type { CSSProperties } from "react"
import { SignStage } from "@marquee/sign-player"
import { ComposePage } from "./pages/Compose"
import { LinesPage } from "./pages/Lines"
import { AppsPage } from "./pages/Apps"
import { LibraryPage } from "./pages/Library"
import { PackPage } from "./pages/Pack"
import { SettingsPage } from "./pages/Settings"
import { ThemeToggle } from "./components/ThemeToggle"
import { PanelBadge } from "./components/PanelBadge"

const NAV = [
  { to: "/", label: "Compose", icon: PenLine, end: true },
  { to: "/lines", label: "Message lines", icon: ListVideo },
  { to: "/apps", label: "Apps", icon: Boxes },
  { to: "/library", label: "Library", icon: Library },
  { to: "/pack", label: "Content pack", icon: Package },
  { to: "/settings", label: "Settings", icon: Settings2 },
]

function Logo() {
  return (
    <div className="flex items-center gap-2.5 px-1 py-1.5">
      <SignStage className="grid size-8 shrink-0 grid-cols-3 place-items-center gap-0.5 rounded-lg p-1.5">
        {["#ffb020", "#ffb020", "#ff5a36", "#ffb020", "#ff5a36", "#ffb020"].map((c, i) => (
          <span key={i} className="size-1.5 rounded-full" style={{ background: c, boxShadow: `0 0 4px ${c}` }} />
        ))}
      </SignStage>
      <div className="grid leading-tight group-data-[collapsible=icon]:hidden">
        <span className="text-sm font-semibold tracking-tight">Umami Marquee Studio</span>
        <span className="text-xs text-muted-foreground">ELITE RGB</span>
      </div>
    </div>
  )
}

export function App() {
  const loc = useLocation()
  const current = NAV.find((n) => (n.end ? loc.pathname === n.to : loc.pathname.startsWith(n.to))) ?? NAV[0]
  return (
    <SidebarProvider style={{ "--sidebar-width": "14rem" } as CSSProperties}>
      <Sidebar collapsible="icon">
        <SidebarHeader>
          <Logo />
        </SidebarHeader>
        <SidebarContent>
          <SidebarGroup>
            <SidebarGroupContent>
              <SidebarMenu>
                {NAV.map((n) => (
                  <SidebarMenuItem key={n.to}>
                    <SidebarMenuButton asChild isActive={n === current} tooltip={n.label}>
                      <NavLink to={n.to} end={n.end}>
                        <n.icon />
                        <span>{n.label}</span>
                      </NavLink>
                    </SidebarMenuButton>
                  </SidebarMenuItem>
                ))}
              </SidebarMenu>
            </SidebarGroupContent>
          </SidebarGroup>
        </SidebarContent>
        <SidebarFooter>
          <PanelBadge />
        </SidebarFooter>
      </Sidebar>
      <SidebarInset>
        <header className="sticky top-0 z-20 flex h-14 shrink-0 items-center gap-2 border-b bg-background/80 px-4 backdrop-blur supports-[backdrop-filter]:bg-background/60">
          <SidebarTrigger className="-ml-1" />
          <Separator orientation="vertical" className="mr-1 data-[orientation=vertical]:h-4" />
          <h1 className="text-sm font-medium">{current.label}</h1>
          <div className="ml-auto flex items-center gap-1">
            <ThemeToggle />
          </div>
        </header>
        <Routes>
          <Route path="/" element={<ComposePage />} />
          <Route path="/lines" element={<LinesPage />} />
          <Route path="/apps" element={<AppsPage />} />
          <Route path="/library" element={<LibraryPage />} />
          <Route path="/library/:tab" element={<LibraryPage />} />
          <Route path="/pack" element={<PackPage />} />
          <Route path="/settings" element={<SettingsPage />} />
          <Route path="*" element={<ComposePage />} />
        </Routes>
      </SidebarInset>
    </SidebarProvider>
  )
}
