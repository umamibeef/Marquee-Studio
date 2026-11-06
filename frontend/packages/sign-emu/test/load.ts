// Test helpers: the sign's fonts from studio/content/fonts, in the firmware's load order.
import { readFileSync } from "node:fs"
import { createHash } from "node:crypto"
import { dirname, join } from "node:path"
import { fileURLToPath } from "node:url"
import { decodePng, FwFont, mqfFont, type Img } from "../src/index.ts"

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..", "..")

export async function loadFonts(names: string[]): Promise<FwFont[]> {
  const out: FwFont[] = []
  for (const n of names) {
    const b = new Uint8Array(readFileSync(join(ROOT, "studio", "content", "fonts", n)))
    out.push(n.endsWith(".mqf") ? await mqfFont(b, n) : new FwFont(await decodePng(b), n))
  }
  return out
}

export function sha(im: Img): string {
  return createHash("sha256").update(Buffer.from(im.d.buffer, im.d.byteOffset, im.d.byteLength)).digest("hex")
}

export function fixtures(name: string) {
  return JSON.parse(readFileSync(join(dirname(fileURLToPath(import.meta.url)), name), "utf8"))
}
