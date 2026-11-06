// strftime for the Studio's clock format help (the codes our firmware's apps understand: firmware/ext/apps.c
// strftime_, sign-emu appvm.ts signStrftime), in local time, with the code table and ready-made formats.

const DAYS = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
const MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
  "November", "December"]
const p2 = (n: number) => String(n).padStart(2, "0")

/** The conversions the apps' fmt_time (firmware/ext/apps.c strftime_) understands: [code, what, example at 2026-11-04 15:07:09]. */
export const STRFTIME_CODES: [string, string, string][] = [
  ["%H", "hour, 24 h, 2 digits", "15"],
  ["%k", "hour, 24 h, space-padded", "15"],
  ["%I", "hour, 12 h, 2 digits", "03"],
  ["%l", "hour, 12 h, no padding", "3"],
  ["%M", "minute", "07"],
  ["%S", "second", "09"],
  ["%p", "AM or PM", "PM"],
  ["%P", "am or pm", "pm"],
  ["%R", "%H:%M", "15:07"],
  ["%T", "%H:%M:%S", "15:07:09"],
  ["%a", "weekday, short", "Wed"],
  ["%A", "weekday", "Wednesday"],
  ["%u", "weekday number, Monday = 1", "3"],
  ["%w", "weekday number, Sunday = 0", "3"],
  ["%d", "day of the month, 2 digits", "04"],
  ["%e", "day of the month, space-padded", " 4"],
  ["%j", "day of the year, 3 digits", "308"],
  ["%b", "month, short", "Nov"],
  ["%B", "month", "November"],
  ["%m", "month number, 2 digits", "11"],
  ["%y", "year, 2 digits", "26"],
  ["%Y", "year", "2026"],
  ["%F", "%Y-%m-%d", "2026-11-04"],
  ["%D", "%m/%d/%y", "11/04/26"],
  ["%%", "a % sign", "%"],
]

/** Ready-made clock formats: [label, format]. */
export const CLOCK_PRESETS: [string, string][] = [
  ["24 hour", "%H:%M"],
  ["24 hour with seconds", "%H:%M:%S"],
  ["12 hour", "%l:%M %p"],
  ["12 hour with seconds", "%l:%M:%S %p"],
  ["Day and time", "%a %H:%M"],
  ["Day and 12 h time", "%a %l:%M %p"],
  ["Short date", "%a %d %b"],
  ["Long date", "%A %e %B"],
  ["Date and time", "%d %b %H:%M"],
  ["ISO date", "%F"],
  ["US date", "%D"],
  ["Day of the year", "Day %j of %Y"],
]

/** strftime's common conversions, in local time. */
export function strftime(fmt: string, t: Date): string {
  return fmt.replace(/%(.)/g, (m, c: string) => {
    const h = t.getHours()
    switch (c) {
      case "H": return p2(h)
      case "M": return p2(t.getMinutes())
      case "S": return p2(t.getSeconds())
      case "I": return p2(h % 12 || 12)
      case "l": return String(h % 12 || 12)
      case "p": return h < 12 ? "AM" : "PM"
      case "P": return h < 12 ? "am" : "pm"
      case "k": return String(h).padStart(2, " ")
      case "j": {
        const y0 = new Date(t.getFullYear(), 0, 1)
        const yd = Math.round((new Date(t.getFullYear(), t.getMonth(), t.getDate()).getTime() - y0.getTime()) / 86400000)
        return String(yd + 1).padStart(3, "0")
      }
      case "u": return String(t.getDay() || 7)
      case "w": return String(t.getDay())
      case "F": return `${t.getFullYear()}-${p2(t.getMonth() + 1)}-${p2(t.getDate())}`
      case "D": return `${p2(t.getMonth() + 1)}/${p2(t.getDate())}/${p2(t.getFullYear() % 100)}`
      case "a": return DAYS[t.getDay()].slice(0, 3)
      case "A": return DAYS[t.getDay()]
      case "b": return MONTHS[t.getMonth()].slice(0, 3)
      case "B": return MONTHS[t.getMonth()]
      case "d": return p2(t.getDate())
      case "e": return String(t.getDate()).padStart(2, " ")
      case "m": return p2(t.getMonth() + 1)
      case "y": return p2(t.getFullYear() % 100)
      case "Y": return String(t.getFullYear())
      case "T": return `${p2(h)}:${p2(t.getMinutes())}:${p2(t.getSeconds())}`
      case "R": return `${p2(h)}:${p2(t.getMinutes())}`
      case "%": return "%"
      default: return m
    }
  })
}

