// Records the animated Umami Marquee Studio demo for the README: docs/media/studio-demo.webp.
//   cd frontend && npm run build && node scripts/readme-demo.mjs
// Everything shown comes from studio/demo_content.py (vendored free fonts, animations drawn in code),
// never from the sign. The studio runs on its own port with default settings, and sending to a serial port is
// blocked, so nothing on this machine or on a sign is touched.
import { chromium } from "@playwright/test"
import { spawn, execFileSync } from "node:child_process"
import fs from "node:fs"
import os from "node:os"
import path from "node:path"
import { fileURLToPath } from "node:url"

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..")
const PY = fs.existsSync(path.join(REPO, "venv/bin/python")) ? path.join(REPO, "venv/bin/python") : "python3"
const OUT = path.join(REPO, "docs/media/studio-demo.webp")
const PORT = 8761
const BASE = `http://127.0.0.1:${PORT}`
const FPS = 7
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "studio-demo-"))
execFileSync(PY, [path.join(REPO, "studio/demo_content.py"), path.join(tmp, "content")], { stdio: "inherit" })

const server = spawn(PY, ["server.py"], {
  cwd: path.join(REPO, "studio"),
  env: {
    ...process.env,
    MARQUEE_STUDIO_CONTENT: path.join(tmp, "content"),
    MARQUEE_STUDIO_CONFIG: path.join(tmp, "config.json"), // missing file: the studio's defaults
    MARQUEE_STUDIO_PORT: String(PORT),
  },
  stdio: "ignore",
})
const stop = () => { try { server.kill() } catch {} }
process.on("exit", stop)

for (let i = 0; ; i++) {
  try { if ((await fetch(BASE + "/")).ok) break } catch {}
  if (i > 60) throw new Error("the studio did not start")
  await sleep(250)
}

const browser = await chromium.launch()
const page = await browser.newPage({ viewport: { width: 1280, height: 760 }, deviceScaleFactor: 1, colorScheme: "dark" })
await page.route("**/api/send_rs485", (r) => r.fulfill({ status: 403, body: '{"ok":false}' }))
await page.goto(BASE + "/", { waitUntil: "networkidle" })
await sleep(1500)

// Frames are grabbed continuously while the script below drives the page.
const framesDir = path.join(tmp, "frames")
fs.mkdirSync(framesDir)
let recording = true
const times = []
const grab = (async () => {
  const t0 = Date.now()
  while (recording) {
    const t = Date.now()
    await page.screenshot({ path: path.join(framesDir, `${String(times.length).padStart(4, "0")}.png`) })
    times.push(t - t0)
    await sleep(Math.max(0, 1000 / FPS - (Date.now() - t)))
  }
})()

const box = page.getByPlaceholder("Type your message")
await box.click()
await box.fill("")
await box.pressSequentially("JACKPOT WINNER  $1,000 ", { delay: 55 })
await sleep(700)

await page.getByRole("button", { name: "Insert emoji" }).click()
await sleep(400)
await page.getByPlaceholder("Search emoji").pressSequentially("cherries", { delay: 70 })
await sleep(450)
await page.getByRole("option").first().click()
await sleep(1200)

await page.getByRole("tab", { name: "Letters" }).click()
await sleep(500)
await page.getByRole("radio", { name: /effect00/i }).first().click()
await sleep(1800)

await page.getByRole("tab", { name: "Background" }).click()
await sleep(500)
await page.getByRole("radio", { name: /effect01/i }).first().click()
await sleep(1800)

await page.getByRole("tab", { name: "Font" }).click()
await sleep(500)
await page.getByRole("radio", { name: /fonte02/i }).first().click()   // another font: native emoji show with any font
await sleep(2000)

recording = false
await grab
await browser.close()
stop()

const durations = times.map((t, i) => (i + 1 < times.length ? times[i + 1] - t : 1000 / FPS))
fs.writeFileSync(path.join(tmp, "durations.json"), JSON.stringify(durations))
fs.mkdirSync(path.dirname(OUT), { recursive: true })
execFileSync(PY, [path.join(REPO, "frontend/scripts/frames-to-webp.py"), framesDir, path.join(tmp, "durations.json"), OUT],
  { stdio: "inherit" })
fs.rmSync(tmp, { recursive: true, force: true })
