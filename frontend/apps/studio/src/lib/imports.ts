import { toast } from "@marquee/ui/lib/toast"
import { api } from "./api"

const IMAGE = /\.(gif|png|apng|webp)$/i

/** Add dropped or chosen animation files: .flc as is (checked), GIF / APNG / WebP converted to FLC. */
export async function addAnimationFiles(
  files: File[],
  opts: { name?: string; fit?: string; depth?: string } = {},
): Promise<string[]> {
  const added: string[] = []
  for (const f of files) {
    const fd = new FormData()
    fd.append("file", f)
    try {
      if (/\.flc$/i.test(f.name)) {
        fd.append("kind", "animations")
        const r = await api.uploadAsset(fd)
        added.push(r.name)
        toast.success(`Added ${r.name}`)
      } else if (IMAGE.test(f.name) || f.type.startsWith("image/")) {
        fd.append("name", opts.name || f.name)
        fd.append("fit", opts.fit || "contain")
        fd.append("depth", opts.depth || "16")
        const r = await api.importAnimation(fd)
        added.push(r.name)
        toast.success(`Converted to ${r.name}`, {
          description: `${r.frames_in} frames → ${r.frames_out} at ${r.speed_ms} ms`,
        })
      } else {
        toast.error(`${f.name}: not an animation`, { description: "Use .flc, .gif, animated .png or .webp" })
      }
    } catch (e) {
      toast.error(`Could not add ${f.name}`, { description: (e as Error).message })
    }
  }
  return added
}

/** Add dropped font PNGs (16 x 16 glyph grids) to the font folder. */
export async function addFontFiles(files: File[]): Promise<string[]> {
  const added: string[] = []
  for (const f of files) {
    if (!/\.png$/i.test(f.name)) {
      toast.error(`${f.name}: not a font`, {
        description: "Drop a sign font PNG, or use Import a free pixel font for BDF / TTF.",
      })
      continue
    }
    const fd = new FormData()
    fd.append("file", f)
    fd.append("kind", "fonts")
    try {
      const r = await api.uploadAsset(fd)
      added.push(r.name)
      toast.success(`Added ${r.name}`)
    } catch (e) {
      toast.error(`Could not add ${f.name}`, { description: (e as Error).message })
    }
  }
  return added
}
