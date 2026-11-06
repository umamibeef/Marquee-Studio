# Ideas for the marquee (research notes, 2026-10-31)

These are ideas for the messaging feature (docs/PORTAL-PLAN.md), drawn from what people build and use with LED
marquees, pixel clocks, split-flap boards, stream tickers and venue message boards. Each idea names the sign
capability it uses, the role that uses or controls it, the effort, and the risk.

Status: ideas only. Nothing here is built or tested on the sign.

## Limits that shape every idea

- Panel size is not confirmed. The content on the sign was made for 80 x 16, and the firmware supports 26 models,
  from 80 x 16 to 200 x 80. The ideas below assume only one line of scrolling text, sometimes with a picture.
  A big panel would allow more (two lines, album art, a bigger clock), but do not plan for it yet.
- Live text is not stored. The studio sends an RS-485 jackpot record, and the sign shows it until the next record
  or a restart. Every "mode" below is therefore a studio feature: the studio's rotation keeps sending the text.
- Short texts must scroll. A record text that fits the panel shows junk (docs/MESSAGE-MARKUP.md), so a clock or a
  one-word shoutout must be padded or marked up so it scrolls. `core.record_text_scrolls` checks this.
- Sound and relays are tied to the jackpot-hit path in the firmware (menu items "JPHit Vol.", "Graphic Sound",
  "Relay Polarities", "Relay Timeout", docs/MENU.md). It is not confirmed whether the studio's live send can
  trigger them, or how. Ideas that use them are marked "unverified path".
- NVRAM paths (attract lines, controller messages) stay out of scope. Ideas that need them are marked.

Risk key:
- NVRAM: needs writes to the sign's NVRAM (out of scope until a sign-side test).
- SOUND: may annoy; needs volume and quiet-hour limits.
- RELAY: switches real hardware; needs care.
- FLASH: can break the three-flashes rule (see "Things to avoid or gate").
- FEED: needs an internet feed or API key.
- OPEN: text from outside the household; needs moderation.

## The ideas

### Social and messaging

| # | Idea | What it looks like on the sign | Capability used | Role | Effort | Risk |
|---|---|---|---|---|---|---|
| 1 | Shoutout | "Happy anniversary, Sam!" slides in from the right, holds, slides out. Poster picks one of a few colour themes. | Markup (slide, hold), solid colours, fonts | Poster writes, Moderator approves | S | – |
| 2 | Emoji reactions | A message ends with a heart or party emoji. Viewers tap a reaction in the studio; the next pass shows "❤️ x4". | Native emoji (from-source firmware), markup | Viewer reacts, Poster writes | M | Emoji need the from-source firmware with native emoji on the sign |
| 3 | Birthday mode | For one day: the person's name in a rainbow FLC fill, a cake emoji, and a birthday line every few minutes in the rotation. | FLC effect fills (FG), colour emoji, rotation schedule | Moderator sets the date; Posters add wishes | S | – |
| 4 | Guestbook | Party guests sign in (or use a guest link); each approved line scrolls once with "– name". The list is kept as a keepsake. | Markup, fonts, rotation | Guest = Poster; Moderator approves | M | OPEN if guests use a QR link |
| 5 | Polls / votes | "Pizza or tacos? 🍕 7 – 🌮 5". The count updates as votes come in. The winner gets a wipe. | Markup, colours, red/yellow/green wipe, emoji | Moderator opens the poll; Viewers vote | M | – |
| 6 | Kudos / thanks wall | "Thanks Alex for fixing the wifi" in a calm colour, low priority, always in rotation for a week. | Markup, priority and schedule | Poster | S | – |
| 7 | "Message of the day" pin | One pinned line between every other message, for example house rules or "Welcome, guests!". | Rotation pin, markup | Moderator | S | – |
| 8 | Dedications ("this one is for…") | A short line with a music-note emoji, shown before a song or toast. | Markup, emoji | Poster | S | – |
| 9 | Notes between people | "Mum: the dog is fed." Each household member has a colour. Expires after a set time. | Solid colours per author, schedule expiry | Poster | S | – |

### Information

| # | Idea | What it looks like on the sign | Capability used | Role | Effort | Risk |
|---|---|---|---|---|---|---|
| 10 | Clock and date | "21:45  Sat 31 Oct" scrolls between messages. | Markup, fonts (studio sends the time text) | Admin turns it on | S | Must scroll (short text) |
| 11 | Countdown | "12 days to the holidays", "Doors open in 0:05:00" with rolling digits. | Odometer / digit roll, markup | Moderator sets the target | S | – |
| 12 | Weather | "8°C ☁️ rain at 18:00" in blue for cold, orange for warm. | Colours, emoji, markup | Admin sets the feed | M | FEED |
| 13 | Transit departures | "Bus 14 in 3 min · 17 min". Like the home train boards people build. | Markup, fonts | Admin sets the feed | M | FEED (local transit API) |
| 14 | Calendar | "Today: dentist 15:00 · bins out tonight". | Markup | Admin links a calendar; Moderator checks it | M | FEED; private data on a public sign |
| 15 | Now playing | "♪ Song title – Artist" when music starts (Spotify, Sonos, Home Assistant media player). | Markup, emoji, fonts | Integration (Poster level); Admin sets it up | M | FEED |
| 16 | Sports scores | "HOME 2 – 1 AWAY 67'". A goal makes the score roll and shows a short burst. | Odometer roll for the score, FLC burst | Admin sets the feed | M | FEED; keep the burst under the flash limit |
| 17 | Sunrise / sunset, moon phase | "Sunset 17:48 🌙 waxing". Popular on pixel clocks. | Markup, emoji | Admin | S | Can be computed offline, no feed needed |
| 18 | Word / fact / joke of the day | One line a day from a list or a feed. | Markup | Admin picks the source; Moderator reviews | S | FEED if online; check jokes before showing |
| 19 | Air quality / home sensors | "CO2 1200 ppm – open a window" in yellow. | Colours, markup | Integration | S | Needs Home Assistant |

### Games and fun

| # | Idea | What it looks like on the sign | Capability used | Role | Effort | Risk |
|---|---|---|---|---|---|---|
| 20 | Jackpot "win" for good news | "JACKPOT: Lily passed her driving test!" with the odometer rolling up to a silly amount ("$1,000,000 of pride"). This is what the sign was built for. | Odometer formatter with rolling digits, win animation (FLC) | Moderator (odometer is Moderator+ in §6) | M | SOUND / RELAY if the jackpot path fires them (unverified path) |
| 21 | Slot-machine reveal | Letters spin with the roll effect and stop one by one on the answer: "WINNER: ALEX". Good for raffles and "who does the dishes". | Letter/digit roll effect, markup holds | Moderator starts it | M | – |
| 22 | Daily fortune | "Your fortune: a snack is in your future". Random line from a list, in a fortune-cookie colour. | Markup, colours | Admin keeps the list | S | – |
| 23 | Trivia | Question scrolls, a hold of 20 seconds, then the answer slides down. Score kept in the studio. | Markup (slide, hold) | Moderator runs it; Viewers answer in the studio | M | – |
| 24 | Scoreboard for house games | "Board games: Ana 5 · Ben 3". The leader's number rolls when it changes. | Odometer roll, colours | Moderator | S | – |
| 25 | Retro attract loop | Old casino FLC animations between messages, as the sign did in the casino. | FLC animations, GIF import | Admin | S | Check FLCs for fast flashing |
| 26 | Pixel pets / classic loops | Nyan-cat-style GIFs, DVD logo bounce. These are in the top 25 Tidbyt apps. | GIF import (to FLC) | Moderator uploads (uploads are Moderator+) | S | Licence of the GIF; FLASH |
| 27 | Hourly chime | A short animation on the hour, with an optional chime. | FLC, sound | Admin | S | SOUND (unverified path) |

### Events and party modes

| # | Idea | What it looks like on the sign | Capability used | Role | Effort | Risk |
|---|---|---|---|---|---|---|
| 28 | Party mode | Faster rotation, bigger fonts, effect fills behind the text, guest messages allowed with quick approval. Ends at a set time. | FLC BG fills, fonts, rotation speed, schedule | Admin turns on; Moderators approve | M | OPEN; FLASH |
| 29 | New Year / launch countdown | Last 10 seconds roll down on the odometer, then "HAPPY NEW YEAR" with a burst. | Odometer roll, FLC, markup | Moderator | M | SOUND / RELAY (unverified path) |
| 30 | Wedding / party message wall | Guests post through a link; a Moderator approves; each line shows with the couple's colours. The wedding products add the same review step. | Markup, colours, emoji | Guests = Poster; Moderator approves every line | M | OPEN |
| 31 | Raffle / prize draw | Names spin with the roll effect and stop on the winner, then the jackpot animation. | Roll effect, odometer, FLC | Moderator | M | Only real prizes, no money gambling |
| 32 | Holiday themes | Halloween orange/purple, winter snow FLC behind text. Set by date. | FLC BG fills, colours, schedule | Admin | S | – |
| 33 | Quiet hours / night mode | Dim colours, clock only, no sound, from 23:00 to 07:00. Vestaboard has the same feature. | Colours, rotation schedule | Admin | S | Brightness change may need menu/NVRAM; use dim colours instead |

### Integrations

| # | Idea | What it looks like on the sign | Capability used | Posting role | Moderation needs | Effort | Risk |
|---|---|---|---|---|---|---|---|
| 34 | Home Assistant alerts | "Washer done", "Door open", "Package at the door" interrupt the rotation, then it resumes. The pixel-clock community does this a lot. | Markup, colours, emoji | Service account, Poster level, but with a "send live" right for a fixed list of templates | Templates are approved once; free text from HA goes to the queue | M | – |
| 35 | Webhook / REST | Any script posts `{"text": ...}` with a token. | Markup | One token per service, Poster level | Goes to the queue unless the token has Moderator rights | S | Token leak = anyone can post |
| 36 | Discord / Slack bot | `/sign Happy Friday!` in a chat channel. The bot replies "queued" or "shown". | Markup, emoji | The bot is one account; each chat user maps to a studio role | Messages from unknown chat users go to the queue | M | OPEN if the server is public |
| 37 | IFTTT / MQTT | New follower, a new video, a doorbell press: a short line on the sign. | Markup | Service account, Poster level | Fixed templates only | S | FEED |
| 38 | Stream ticker (Twitch / YouTube) | Follows, subs and raids as a short line; a big raid gets the jackpot animation. | Markup, odometer, FLC | Service account | Chat text never goes direct; only event templates | M | OPEN; SOUND |
| 39 | Email / SMS to the sign | Send to an address, the line goes to the queue. A hackerspace sign did this by email, with a queue so no one person hogs it. | Markup | Mapped sender = Poster; unknown sender = rejected or queued | Every message reviewed | M | OPEN; spam |

## Top 10 to build first

1. Shoutout (#1). It is the core of the messaging plan and uses only markup, colours and fonts, which the
   studio already encodes and previews exactly.
2. Clock and countdown in the rotation (#10, #11). The most-installed apps on Tidbyt are clock-like, and no feed is
   needed. It also tests the "short text must scroll" rule early.
3. Jackpot "win" for good news (#20). This is the sign's own trick. The odometer formatter matches the firmware on
   412 cases. Moderator+ only, as the plan already says. Start with no sound and no relay.
4. Birthday mode (#3). A small feature that uses the scheduler, a pinned priority, the FLC effect fills and emoji.
5. Message of the day pin (#7). It exercises pin and priority in the rotation with almost no new UI.
6. Quiet hours / night mode (#33). Needed before any sound or party feature. Vestaboard ships this as a basic
   feature for the same reason.
7. Home Assistant / webhook alerts with fixed templates (#34, #35). One service account, template approval
   once, send-live for templates only. It fits the role model and is the most common home use for these displays.
8. Slot-machine reveal (#21). It reuses the letter/digit roll effect and markup holds. Good for raffles and chores.
9. Polls with emoji (#5). It brings in the Viewer role (they vote) and shows the queue works both ways.
10. Guestbook / party wall (#4, #30). This is the main "many posters" case. It needs the moderation queue,
    rate limits and the block-list from §5 of the plan, so build it after those.

Weather, transit and now-playing are popular too, but each needs an internet feed and API keys. The hackerspace sign
"Give Me A Sign!" has a good pattern for this: the sign side never calls APIs, and a home server (Home Assistant)
fetches the data and pushes text. Here that means the studio receives text from HA and holds no weather or Spotify
keys.

## Things to avoid or gate

- Public, unmoderated posting. Every open sign gets abused. A Toronto transit sign was reprogrammed with a slur
  using a cheap remote, and the fix was a password. Venue shoutout boards add a profanity filter and a moderation
  queue.
  - Keep the plan's default: Posters wait for approval. Guests (QR link) are Posters at most.
  - Rate limit per user, with a cap so one person cannot hog the rotation.
  - The audit log must record the guest link or token used.
- Flashing and photosensitivity (WCAG 2.3.1). Nothing may flash more than three times in any one-second
  period, unless the flashes are below the general flash and red flash thresholds. A red flash is any pair of
  opposing transitions involving saturated red. The size threshold is a quarter of a 10° field of view (W3C uses a
  341 x 256 pixel block at normal viewing distance). A whole LED marquee seen from across a room easily fills that,
  so treat every full-sign flash as above the threshold. UK epilepsy charities name 3 to 30 flashes a second as the
  common trigger range.
  - The sign's flash setting (checked in the firmware, 2026-10-31): "Flash On" / "Flash Off" are stored in
    milliseconds (menu range 25 to 6375) and converted to 40 fps frames by / 25 (`jackpot_controller_init`). A
    cycle (on + off) of at least 334 ms stays within 3 flashes a second. The studio's old default (5 / 5) was below
    the sign's minimum. The default is now 500 / 500 ms, the Design tab takes milliseconds, and the preview warns
    above 3 flashes a second (`core.flash_warnings`). For Posters, allow flash only with a cycle of 334 ms or more,
    or not at all.
  - Check FLC animations and GIF imports for fast light/dark or red changes before they enter the rotation.
  - The red/yellow/green wipe changes colour slowly (one row every 4 frames), so it counts as a wipe. Still, do not
    put red full-panel strobes in "win" animations.
- Sound. Keep it off by default. Admin-only, with a volume cap, short clips (a few seconds), and quiet hours.
  The sound path from the studio is not confirmed yet.
- Relays. The two jackpot relays switch real hardware (a beacon, a bell, or whatever is wired to them). Admin
  only, and never from a Poster, a guest or an integration. Know what is wired before any test. The menu has a
  relay timeout (up to 300 s) and polarities; do not change them as part of a "fun" feature.
- NVRAM writes. Anything that would make a message survive a restart (attract lines, controller messages,
  brightness via the menu) writes NVRAM. Keep it out until the sign-side test in BACKLOG.
- Private data. Calendar, location and "who is home" lines are visible to anyone in the room. Keep them opt-in.
- Money and the gambling look. The jackpot animation is fun for good news. Do not attach real money, bets or prizes
  with value to the odometer, since that can become real gambling under local law.
- Licences. Apple emoji stay local-only (THIRD-PARTY.md). GIFs from the internet often have no licence.

## Sources

Pixel displays and apps
- Tidbyt app ranking by installs (Weather, Clock, Stocks, Sunrise Sunset, Baseball, ... Messages #16, Countdown, Transit):
  https://discuss.tidbyt.com/t/app-ranking/3333
- Tidbyt community apps (now continued as Tronbyt): https://github.com/tidbyt/community, https://github.com/tronbyt/apps
- LaMetric Time apps and IFTTT triggers (subscriber counts, countdowns, notifications):
  https://9to5mac.com/2017/05/09/lametric-time-wi-fi-clock-video/, https://ifttt.com/connect/lametric/youtube

Home Assistant and status boards
- AWTRIX / Ulanzi blueprints (notifications that interrupt the display, sensor apps in rotation):
  https://github.com/smarthomejunkie/Ulanzi-Awtrix-BluePrints
- EspHoMaTriX (8x32 clock/status display, HA alert screen): https://github.com/andrewjswan/EspHoMaTriXv2,
  https://community.home-assistant.io/t/esphomatrix-a-simple-clock-status-display/425325
- ESPTimeCast (doorbell / "washing machine done" alerts that restore the display): https://github.com/mfactory-osaka/ESPTimeCast
- Washer-done detection with HA: https://www.github.com/lostinthebuild/S01E02-state-machine-washer-notifier-II
- Give Me A Sign! (hackerspace info sign, data only via MQTT, no API keys on the sign):
  https://romkey.com/hackerspace-suite/, https://github.com/romkey/give-me-a-sign
- Nottingham Hackspace (door events over MQTT, LED matrix shows who arrives):
  https://wiki.nottinghack.org.uk/wiki/HackSpace_Instrumentation

Transit and now playing
- Home train departure boards: https://github.com/jonmorrissmith/rpitraindisplay,
  https://hackaday.com/2026/07/23/a-train-departure-board-for-the-home/, https://github.com/jdamcd/arrivals-led
- Spotify / now-playing matrices: https://github.com/kylejohnsonkj/rpi-spotify-matrix-display,
  https://hackster.io/news/evan-nishi-s-spotify-display-offers-a-low-res-raspberry-pi-zero-w-powered-look-at-what-s-playing-f0626a090b31,
  https://github.com/feram18/now-playing

Split-flap and message boards
- Vestaboard (send from an app, channels, quiet hours, mute, guest send): https://www.vestaboard.com/plus,
  https://www.vestaboard.com/help/quiet-hours, https://www.vestaboard.com/help/muting, https://www.vestaboard.com/help/guest-send
- Split-flap as a Spotify display: https://hackaday.com/2020/02/11/airport-split-flap-letters-carry-on-as-spotify-display
- SOLARIBOARD (airport-style message board): https://medium.com/@tart2000/project-solariboard-realtime-airport-like-message-board-36c33fef1587
- Venue shoutout board (QR posting, profanity filter modes, moderation queue, specials between shoutouts):
  https://theflappening.pages.dev/venue

Streams and chat
- Twitch chat ticker on two 64x32 panels, pulse on each message, alerts via MQTT:
  https://cabinetterror.blogspot.com/2026/03/twitch-led-screen-write-up.html
- Twitch events (follows, subs, bits, raids) to a serial display: https://github.com/Stukeley/ArduinoTwitchBot
- On-air sign: https://learn.adafruit.com/3d-printed-iot-on-air-sign-for-twitch
- Channel-point rewards limited by user level (Lumia Stream): https://forum.lumiastream.com/t/lights-wont-trigger-for-those-not-subbed-via-twitch/150
- Discord bot driving an RGB matrix over UART: https://github.com/kolpim13/Matrix_ChatBot

Events and venues
- Wedding social walls (SMS / QR posting, review before display): https://everwall.com/blog/how-couples-use-social-walls-weddings/,
  https://pcdj.com/enhance-weddings-and-events-with-jammtext-text-to-screen-software/, https://specialevents.com/node/3152
- Bar signage (specials, birthday shout-outs): https://www.loop.tv/learn/5-examples-of-digital-signage-for-bars-and-pubs

Abuse and moderation
- Toronto transit LED signs reprogrammed with a cheap remote:
  https://www.computerworld.com/article/1689763/hacker-hits-toronto-transit-message-system-jabs-prime-minister.html
- Skullspace email-to-sign with a queue and logging: https://groups.google.com/a/skullspace.ca/g/discuss/c/yEMMExhaaBA
- Old serial LED signboard put online: https://hackaday.com/2016/12/26/iot-ifying-an-old-led-signboard

Flashing and photosensitivity
- WCAG 2.2, Understanding SC 2.3.1 Three Flashes or Below Threshold:
  https://www.w3.org/WAI/WCAG22/Understanding/three-flashes-or-below-threshold.html
- Epilepsy Society (UK), photosensitive epilepsy, 3 to 30 Hz: https://epilepsysociety.org.uk/node/3176,
  https://epilepsysociety.org.uk/node/3618

Casino background
- Mikohn Gaming: https://en.wikipedia.org/wiki/Mikohn_Gaming
- Odometer / paced jackpot displays are allowed, with polling delay noted (Arkansas rule): https://state-rules.com/arkansas/car/355-1802/
