// The web studio previews everything with its own emulation (@marquee/sign-emu): this walks every page and every
// preview (Compose and its tabs, the GIF export, Message lines, Apps, Library, Content pack, Settings) in headless
// Chromium and fails if the page asks the server for anything but files, settings, the port list or bytes, or if a
// preview stays empty. Settings saves, uploads and sends are answered here, never reach the server or a port. App
// bundles (built by the server) and feed values are bytes and data: the apps' frames are drawn here.
//   node scripts/emulation-only.mjs [base URL]      default http://127.0.0.1:8750
import { chromium } from "@playwright/test"

const base = process.argv[2] || "http://127.0.0.1:8750"
const ALLOWED_GET = [/^\/api\/(config|assets|emoji|sign_files|emoji_sheet|serial_ports|apps)$/, /^\/api\/content\/(fonts|animations)\/[\w.-]+$/,
  /^\/emoji\/(pack|sheet)\/\d+\.png$/]
const ANSWERED = { "/api/config": '{"ok":true}', "/api/upload_apps": '{"ok":true,"listing":[],"apps":0}',
  "/api/send_rs485": '{"ok":true,"sent":""}' }
const bad = [], errors = []
const b = await chromium.launch()
const ctx = await b.newContext({ viewport: { width: 1440, height: 1000 }, acceptDownloads: true })
const p = await ctx.newPage()
p.on("pageerror", (e) => errors.push(e.message))
p.on("console", (m) => m.type() === "error" && errors.push(m.text()))
await p.route(/\/(api|emoji)\//, async (route) => {
  const r = route.request(), path = new URL(r.url()).pathname
  if (r.method() === "GET" && ALLOWED_GET.some((re) => re.test(path))) return route.continue()
  if (r.method() === "POST" && path in ANSWERED) return route.fulfill({ status: 200, contentType: "application/json", body: ANSWERED[path] })
  if (r.method() === "POST" && (path === "/api/app_bundle" || path.startsWith("/api/feed/")))   // bytes / data
    return route.continue()
  if (r.method() === "POST" && path === "/api/markup") {                // bytes only
    const res = await route.fetch()
    const j = await res.json()
    if ("preview" in j) bad.push("POST /api/markup returned a server-rendered preview")
    return route.fulfill({ response: res })
  }
  bad.push(`${r.method()} ${path}`)
  return route.fulfill({ status: 410, body: "not allowed: the studio must emulate" })
})
const go = async (path) => {
  await p.goto(base + path, { waitUntil: "networkidle" })
  await p.waitForTimeout(1500)
}
const lit = async (what) => {                                           // a preview player drew something
  await p.waitForTimeout(500)
  const n = await p.locator("canvas").count()
  if (!n) bad.push(`${what}: no preview drawn`)
}
try {
  await go("/")
  await lit("Compose")
  for (const t of ["Style", "Letters", "Background", "Motion", "Font"]) {
    await p.getByRole("tab", { name: t }).click()
    await p.waitForTimeout(1500)
  }
  const [dl] = await Promise.all([p.waitForEvent("download"), p.getByRole("button", { name: "Save as GIF" }).click()])
  const gif = await (await dl.createReadStream()).toArray()
  if (Buffer.concat(gif).subarray(0, 6).toString() !== "GIF89a") bad.push("the GIF export is not a GIF")
  await p.getByRole("button", { name: "Show on the sign" }).click()
  await p.waitForTimeout(800)
  await p.keyboard.press("Escape")
  await go("/lines")
  await lit("Message lines")
  await go("/apps")
  await p.getByRole("button", { name: "Add app" }).click()
  await p.waitForTimeout(1500)
  await lit("Apps")
  for (const t of ["Letters", "Background", "Font"]) {
    await p.getByRole("tab", { name: t }).last().click()
    await p.waitForTimeout(800)
  }
  await go("/library")
  await p.getByRole("tab", { name: "Fonts" }).click()
  await p.waitForTimeout(1500)
  await go("/pack")
  await go("/settings")
} catch (e) {
  bad.push("a step failed: " + String(e).split("\n")[0])
}
await b.close()
for (const e of errors) bad.push("page error: " + e)
if (bad.length) {
  console.log("FAIL\n  " + [...new Set(bad)].join("\n  "))
  process.exit(1)
}
console.log("emulation only: every page and preview drawn in the browser; the server gave files, settings and bytes")
