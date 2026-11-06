// The Studio's strftime (format help, local time) and the firmware's (appvm.ts signStrftime, a port of
// firmware/ext/apps.c strftime_, on sign seconds) agree on every code, over years of dates (UTC here, so the two
// clocks are the same).
import { test } from "node:test"
import assert from "node:assert/strict"
process.env.TZ = "UTC"
const { signStrftime, strftime, STRFTIME_CODES } = await import("../src/index.ts")

test("every code, every few days for 30 years", () => {
  const fmt = STRFTIME_CODES.map(([c]) => c).join("|")
  const bytes = new TextEncoder().encode(fmt)
  for (let t = 946684800; t < 946684800 + 30 * 365 * 86400; t += 3 * 86400 + 3601) {
    const want = strftime(fmt, new Date(t * 1000))
    const got = String.fromCharCode(...signStrftime(bytes, t, 1000))
    assert.equal(got, want, `at ${t}`)
  }
})
