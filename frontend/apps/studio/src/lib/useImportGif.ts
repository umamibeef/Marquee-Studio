// An animated GIF (or PNG, APNG, WebP) into the message: imported as an emoji of the sign's set, so it sits inline in
// the text and travels to the sign with the apps (docs/NATIVE-EMOJI.md).
import { useRef, useState } from "react"
import { useQueryClient } from "@tanstack/react-query"
import { toast } from "@marquee/ui/lib/toast"
import { api } from "@/lib/api"

export const IMAGE_TYPES = "image/gif,image/png,image/webp"
const isImage = (f: File) => /^image\/(gif|png|webp|apng)$/.test(f.type) || /\.(gif|png|apng|webp)$/i.test(f.name)

export function useImportGif(onInsert: (text: string) => void) {
  const qc = useQueryClient()
  const [busy, setBusy] = useState(false)
  const insert = useRef(onInsert)                 // the message as it is when the import ends, not when it began
  insert.current = onInsert
  const importFiles = async (files: Iterable<File>) => {
    setBusy(true)
    try {
      for (const f of files) {
        if (!isImage(f)) {
          toast.error(`${f.name}: not a picture`, { description: "Use a GIF, PNG, APNG or WebP." })
          continue
        }
        const fd = new FormData()
        fd.append("file", f)
        fd.append("name", f.name.replace(/\.[^.]+$/, ""))
        try {
          const r = await api.importEmoji(fd)
          if (!r.ok || !r.e) throw new Error(r.error ?? "import failed")
          await qc.invalidateQueries()              // the sheet first, so that the preview can draw it
          insert.current(r.e)
        } catch (e) {
          toast.error(`Could not import ${f.name}`, { description: (e as Error).message })
        }
      }
    } finally {
      setBusy(false)
    }
  }
  return { importFiles, busy }
}
