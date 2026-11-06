import { useEffect, useMemo, useState, type ReactNode } from "react"
import { useQuery } from "@tanstack/react-query"
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from "@marquee/ui/components/ui/dialog"
import { Button } from "@marquee/ui/components/ui/button"
import { Label } from "@marquee/ui/components/ui/label"
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@marquee/ui/components/ui/select"
import { Alert, AlertDescription } from "@marquee/ui/components/ui/alert"
import { Loader2, RefreshCw, Send, TriangleAlert } from "@marquee/ui/lib/icons"
import { toast } from "@marquee/ui/lib/toast"
import { api, useConfig } from "@/lib/api"

const PORT_KEY = "marquee-studio.port.v1"

/** "Send to sign": one RS-485 frame through a USB adapter (/api/send_rs485). */
export function SendDialog({
  payload,
  children,
  description = "The text goes out as a live RS-485 record. The sign draws it with the level's own style.",
  warning,
}: {
  payload: Record<string, unknown>
  children: ReactNode
  description?: string
  warning?: string
}) {
  const [open, setOpen] = useState(false)
  const { data: cfg } = useConfig()
  const ports = useQuery({ queryKey: ["ports"], queryFn: api.serialPorts, enabled: open, staleTime: 0 })
  const [port, setPort] = useState(() => {
    try {
      return localStorage.getItem(PORT_KEY) ?? ""
    } catch {
      return ""
    }
  })
  const [level, setLevel] = useState("0")
  const [busy, setBusy] = useState(false)
  const list = useMemo(() => ports.data?.ports ?? [], [ports.data])
  useEffect(() => {
    // pick a USB serial adapter by default, never a Bluetooth or modem port
    if (list.length && !list.includes(port)) setPort(list.find((p) => /usb|serial|ftdi|rs485|wch/i.test(p)) ?? "")
  }, [list, port])

  const send = async () => {
    setBusy(true)
    try {
      try {
        localStorage.setItem(PORT_KEY, port)
      } catch {
        /* ignore */
      }
      const r = await api.sendRs485({ ...payload, port, level: +level, baud: cfg?.PROTOCOL["RS485 Baud"] ?? 19200 })
      toast.success("Sent to the sign", { description: `${(r.sent ?? "").split(" ").length} bytes on ${port}` })
      setOpen(false)
    } catch (e) {
      toast.error("Could not send", { description: String((e as Error).message) })
    } finally {
      setBusy(false)
    }
  }

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>{children}</DialogTrigger>
      <DialogContent className="sm:max-w-md">
        <DialogHeader>
          <DialogTitle>Send to sign</DialogTitle>
          <DialogDescription>{description}</DialogDescription>
        </DialogHeader>
        <div className="grid gap-4 py-1">
          <div className="grid gap-2">
            <Label htmlFor="send-port">Serial port</Label>
            <div className="flex gap-2">
              <Select value={port} onValueChange={setPort}>
                <SelectTrigger id="send-port" className="flex-1">
                  <SelectValue placeholder={ports.isLoading ? "Looking for adapters…" : list.length ? "Choose the USB RS-485 adapter" : "No serial ports found"} />
                </SelectTrigger>
                <SelectContent>
                  {list.map((p) => (
                    <SelectItem key={p} value={p}>
                      {ports.data?.labels?.[p] ?? p}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
              <Button variant="outline" size="icon" aria-label="Refresh ports" onClick={() => ports.refetch()}>
                <RefreshCw className={ports.isFetching ? "animate-spin" : ""} />
              </Button>
            </div>
          </div>
          <div className="grid gap-2">
            <Label htmlFor="send-level">Level</Label>
            <Select value={level} onValueChange={setLevel}>
              <SelectTrigger id="send-level" className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {(cfg?.levels ?? [{ name: "Level 1" }]).map((l, i) => (
                  <SelectItem key={i} value={String(i)}>
                    {l.name || `Level ${i + 1}`}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>
          {warning && (
            <Alert>
              <TriangleAlert />
              <AlertDescription>{warning}</AlertDescription>
            </Alert>
          )}
        </div>
        <DialogFooter>
          <Button variant="ghost" onClick={() => setOpen(false)}>
            Cancel
          </Button>
          <Button onClick={send} disabled={!port || busy}>
            {busy ? <Loader2 className="animate-spin" /> : <Send />}
            Send
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
