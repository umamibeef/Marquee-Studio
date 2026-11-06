import { useState } from "react"
import { Button } from "@marquee/ui/components/ui/button"
import { Checkbox } from "@marquee/ui/components/ui/checkbox"
import { Badge } from "@marquee/ui/components/ui/badge"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@marquee/ui/components/ui/card"
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@marquee/ui/components/ui/table"
import { Download, Film, Loader2, Type } from "@marquee/ui/lib/icons"
import { toast } from "@marquee/ui/lib/toast"
import { downloadBlob, useAssets } from "@/lib/api"
import { animLabel } from "@/components/AnimationPicker"
import { fontTitle, originLabel } from "@/components/FontPicker"

export function PackPage() {
  const { data: assets } = useAssets()
  const [sel, setSel] = useState<Set<string>>(new Set())
  const [busy, setBusy] = useState(false)
  const rows = [
    ...(assets?.animations ?? []).map((n) => ({ n, kind: "animation" as const })),
    ...(assets?.fonts ?? []).map((n) => ({ n, kind: "font" as const })),
  ]
  const all = rows.length > 0 && sel.size === rows.length
  const toggle = (n: string, on: boolean) => {
    const s = new Set(sel)
    if (on) s.add(n)
    else s.delete(n)
    setSel(s)
  }
  const build = async () => {
    setBusy(true)
    try {
      const r = await fetch("/api/pack", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ files: [...sel] }),
      })
      if (!r.ok) throw new Error(`${r.status} ${r.statusText}`)
      await downloadBlob(await r.blob(), "content.lfs")
      toast.success("content.lfs built", { description: `${sel.size} files plus the settings` })
    } catch (e) {
      toast.error("Could not build the pack", { description: (e as Error).message })
    } finally {
      setBusy(false)
    }
  }
  return (
    <div className="mx-auto w-full max-w-4xl px-4 py-6 sm:px-6 lg:px-8">
      <Card>
        <CardHeader className="flex flex-row items-start justify-between gap-4">
          <div className="grid gap-1.5">
            <CardTitle>Content pack</CardTitle>
            <CardDescription>An LFS image with your settings, for the sign's DB9 download.</CardDescription>
          </div>
          <Button onClick={build} disabled={busy}>
            {busy ? <Loader2 className="animate-spin" /> : <Download />}
            Build content.lfs
          </Button>
        </CardHeader>
        <CardContent>
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead className="w-10">
                  <Checkbox
                    aria-label="Select all"
                    checked={all ? true : sel.size ? "indeterminate" : false}
                    onCheckedChange={(v) => setSel(v ? new Set(rows.map((r) => r.n)) : new Set())}
                  />
                </TableHead>
                <TableHead>File</TableHead>
                <TableHead>Name</TableHead>
                <TableHead className="text-right">Origin</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {rows.map(({ n, kind }) => {
                const fi = kind === "font" ? assets?.font_info[n] : undefined
                return (
                  <TableRow key={n} data-state={sel.has(n) ? "selected" : undefined}>
                    <TableCell>
                      <Checkbox aria-label={`Include ${n}`} checked={sel.has(n)} onCheckedChange={(v) => toggle(n, !!v)} />
                    </TableCell>
                    <TableCell className="font-mono text-xs">
                      <span className="inline-flex items-center gap-2">
                        {kind === "font" ? <Type className="size-3.5 text-muted-foreground" /> : <Film className="size-3.5 text-muted-foreground" />}
                        {n}
                      </span>
                    </TableCell>
                    <TableCell className="text-muted-foreground">{kind === "font" ? fontTitle(n, fi) : animLabel(assets, n)}</TableCell>
                    <TableCell className="text-right">
                      {kind === "font" ? (
                        <Badge variant={fi?.shareable ? "secondary" : "outline"} className="font-normal">
                          {originLabel(fi)}
                        </Badge>
                      ) : (
                        <Badge variant="outline" className="font-normal text-muted-foreground">
                          {assets?.animation_info[n]?.slot != null ? "effect" : "animation"}
                        </Badge>
                      )}
                    </TableCell>
                  </TableRow>
                )
              })}
            </TableBody>
          </Table>
        </CardContent>
      </Card>
    </div>
  )
}
