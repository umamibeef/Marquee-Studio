import { useRef, useState } from "react"
import { Button } from "@marquee/ui/components/ui/button"
import { Input } from "@marquee/ui/components/ui/input"
import { Label } from "@marquee/ui/components/ui/label"
import { Textarea } from "@marquee/ui/components/ui/textarea"
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle, DialogTrigger } from "@marquee/ui/components/ui/dialog"
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@marquee/ui/components/ui/table"
import { CircleHelp, Smile } from "@marquee/ui/lib/icons"
import { CLOCK_PRESETS, strftime, STRFTIME_CODES } from "@marquee/sign-emu"
import { EmojiPicker } from "@/components/EmojiPicker"
import { Section } from "@/components/Section"

// The Apps page's setting controls: a labelled field, a text input with the sign's emoji picker, and the strftime
// format help.

export function Field({ label, children, htmlFor, className }: { label: string; children: React.ReactNode; htmlFor?: string; className?: string }) {
  return (
    <div className={"grid gap-2 " + (className ?? "")}>
      <Label htmlFor={htmlFor} className="text-muted-foreground">
        {label}
      </Label>
      {children}
    </div>
  )
}


/** A text input with the sign's emoji picker: an emoji goes in at the cursor. */
export function EmojiInput({ value, onChange, className, multiline }: { value: string; onChange: (v: string) => void; className?: string; multiline?: boolean }) {
  const ref = useRef<HTMLInputElement & HTMLTextAreaElement>(null)
  const insert = (s: string) => {
    const el = ref.current
    const a = el?.selectionStart ?? value.length, b = el?.selectionEnd ?? a
    onChange(value.slice(0, a) + s + value.slice(b))
    requestAnimationFrame(() => {
      el?.focus()
      el?.setSelectionRange(a + s.length, a + s.length)
    })
  }
  return (
    <div className={"relative " + (className ?? "")}>
      {multiline ? (
        <Textarea ref={ref} rows={3} className="pr-10" value={value} onChange={(e) => onChange(e.target.value)} />
      ) : (
        <Input ref={ref} className="pr-10" value={value} onChange={(e) => onChange(e.target.value)} />
      )}
      <div className="absolute top-1 right-1">
        <EmojiPicker onPick={(e) => insert(e.e)} onlyNative>
          <Button variant="ghost" size="icon-sm" aria-label="Insert emoji">
            <Smile />
          </Button>
        </EmojiPicker>
      </div>
    </div>
  )
}

/** Every strftime code the sign understands, and ready-made formats to pick from. */
export function FormatCodes({ format, onPick }: { format: string; onPick: (f: string) => void }) {
  const [open, setOpen] = useState(false)
  const now = new Date()
  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>
        <Button variant="outline">
          <CircleHelp /> Format codes
        </Button>
      </DialogTrigger>
      <DialogContent className="max-h-[85vh] overflow-y-auto sm:max-w-2xl">
        <DialogHeader>
          <DialogTitle>Clock format</DialogTitle>
          <DialogDescription>
            The format is strftime: the codes below are replaced by the sign's time, everything else (and emoji) shows as
            typed. Now: <span className="font-mono">{strftime(format, now) || "(nothing)"}</span>
          </DialogDescription>
        </DialogHeader>
        <Section title="Ready-made">
          <div className="grid gap-1.5 sm:grid-cols-2">
            {CLOCK_PRESETS.map(([label, f]) => (
              <Button key={f} variant={f === format ? "default" : "outline"} className="h-auto justify-between gap-3 py-2"
                onClick={() => { onPick(f); setOpen(false) }}>
                <span>{label}</span>
                <span className="font-mono text-xs opacity-80">{strftime(f, now)}</span>
              </Button>
            ))}
          </div>
        </Section>
        <Section title="Codes">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Code</TableHead>
                <TableHead>Gives</TableHead>
                <TableHead>Now</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {STRFTIME_CODES.map(([code, what]) => (
                <TableRow key={code} className="cursor-pointer" onClick={() => onPick(format + code)}>
                  <TableCell className="font-mono">{code}</TableCell>
                  <TableCell>{what}</TableCell>
                  <TableCell className="font-mono whitespace-pre">{strftime(code, now)}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
          <p className="text-sm text-muted-foreground">Click a code to add it to the end of the format.</p>
        </Section>
      </DialogContent>
    </Dialog>
  )
}

