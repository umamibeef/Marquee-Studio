// Screenshots of the studio for design review (headless Chromium). Never sends anything to a serial port.
//   node scripts/screenshots.mjs [base URL] [out dir]     default http://127.0.0.1:8750  /tmp/studio-shots
import { chromium } from "@playwright/test"
import fs from "node:fs"

const base = process.argv[2] || "http://127.0.0.1:8750"
const out = process.argv[3] || "/tmp/studio-shots"
fs.mkdirSync(out, { recursive: true })
const only = process.env.SHOTS ? process.env.SHOTS.split(",") : null

const browser = await chromium.launch()
const errors = []
for (const [w, h] of [[1440, 900], [1280, 800]]) {
  const ctx = await browser.newContext({ viewport: { width: w, height: h }, deviceScaleFactor: 1 })
  const page = await ctx.newPage()
  page.on("pageerror", (e) => errors.push(`${w}: ${e.message}`))
  page.on("console", (m) => m.type() === "error" && errors.push(`${w} console: ${m.text()}`))
  // Safety: refuse any send request.
  await page.route("**/api/send_rs485", (r) => r.fulfill({ status: 403, body: '{"ok":false,"error":"blocked in screenshots"}' }))
  const shot = async (name, fn) => {
    if (only && !only.includes(name)) return
    await fn()
    await page.waitForTimeout(1200)
    await page.screenshot({ path: `${out}/${name}-${w}x${h}.png` })
  }
  const go = async (path) => {
    await page.goto(base + path, { waitUntil: "networkidle" })
    await page.waitForTimeout(800)
  }
  await shot("compose", () => go("/"))
  for (const t of ["Letters", "Background", "Motion", "Font"]) {
    await shot(`compose-${t.toLowerCase()}`, async () => {
      await go("/")
      await page.getByRole("tab", { name: t }).click()
      await page.waitForTimeout(1500)
    })
  }
  await shot("compose-emoji", async () => {
    await go("/")
    await page.getByRole("button", { name: "Insert emoji" }).click()
    await page.waitForTimeout(1200)
  })
  await shot("compose-send", async () => {
    await go("/")
    await page.getByRole("button", { name: "Send to sign" }).click()
  })
  await shot("lines", () => go("/lines"))
  await shot("library-animations", () => go("/library/animations"))
  await shot("library-fonts", () => go("/library/fonts"))
  await shot("pack", () => go("/pack"))
  await shot("settings", () => go("/settings"))
  await shot("compose-light", async () => {
    await go("/")
    await page.getByRole("button", { name: "Toggle theme" }).click()
    await page.waitForTimeout(500)
  })
  await page.evaluate(() => localStorage.setItem("theme", "dark"))
  await ctx.close()
}
await browser.close()
if (errors.length) console.log("ERRORS:\n" + [...new Set(errors)].join("\n"))
console.log("screenshots in", out)
