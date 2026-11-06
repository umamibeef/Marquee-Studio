# Marquee Portal (messaging) and Umami Marquee Studio: plan (draft, 2026-10-31)

Goal: people post messages to the sign through Marquee Portal, a web interface deployed with the sign.
Umami Marquee Studio (the content designer) works standalone and inside the portal. Depending on the poster's
authorization level, a message is shown on the sign at once, waits for approval, or cannot be posted at all. The
studio's existing pieces do the drawing and sending: the markup encoder, the firmware-exact preview, and the RS-485
send.

Status: planning only. Nothing here is built. Open questions are at the end.

## 1. Where we start from

- Studio today: Flask on `127.0.0.1:8750`, one user, no accounts. `/api/send_rs485` sends straight to the sign.
  Anyone who can reach the port can send.
- How text reaches the sign live: the only path the studio uses is an RS-485 jackpot record whose location
  text the sign shows. A long marked-up text scrolls through message line 16 (docs/MESSAGE-MARKUP.md).
  - The sign shows it but does not store it: the next record or a restart replaces it.
  - It does not write the sign's NVRAM or flash, which keeps it inside the project's safety rules.
- Paths not used for now: attract lines (MESSAGE_TABLE) and controller messages (TEXT_LINE_DATABASE). Both are
  stored in NVRAM, and the controller-message path has not been tested on the sign. The plan keeps them out of
  scope until a sign-side test shows they are safe (see BACKLOG). A message is therefore on the sign only while
  the studio keeps sending it.

## 2. Levels and what they may do (decided 2026-10-31)

There are several levels, from anonymous up through signed-in accounts. Review can be switched on or off, and in
most cases people identify themselves with an account. The levels are therefore configurable policies rather than
a fixed table: each level has the same set of switches, and the Admin sets them.

Levels (each includes everything the levels below it may do):

| Level | Who | Default policy |
|---|---|---|
| Anonymous | anyone with the link, not signed in | off. When an Admin turns it on: posts are always reviewed, need a CAPTCHA, have a short length, plain text only, and a strict rate limit per IP address |
| Member | signed in with an account | may post; review on |
| Trusted | members an Admin or Moderator has promoted | may post; review off (straight into the rotation); more styles |
| Moderator | | reviews others' posts, schedules, sends live, pins |
| Admin | | users and levels, the switches below, sign settings |

Switches per level (Admin-editable, kept in the database, applied by the server):
- may post (yes / no);
- review required (yes / no). "No" means the post goes straight into the rotation;
- rate limit (posts per hour; for Anonymous also counted per IP address);
- maximum length, and whether markup, colours, effects, emoji, fonts and odometer amounts are allowed (§6);
- CAPTCHA required (forced on for Anonymous);
- how long a post may stay in the rotation (e.g. Members 1 day, Trusted 7 days).

Global switches:
- posting open / closed (an off switch for the whole public side);
- review everything (forces review on for every level, e.g. during an event or after abuse);
- sign-ups: open, invite-only, or closed. The recommended start is invite-only. New accounts from open sign-up
  start as Members, with review on.

Rules that do not change with the switches:
- An approval covers one exact version. Editing an approved post sends it back to review unless the author's level
  needs no review.
- Nobody approves their own post unless their level could have posted it without review.
- There is always at least one Admin.
- Moderators and Admins can remove anything at any time. Removing a post also takes it off the sign at the next
  rotation step. A "clear the sign now" button sends a blank record at once.

## 3. What a message is

```
message: id, author, created, status (draft | pending | approved | rejected | expired | removed),
         text, style {font, fg, bg, outline, effects, markup segments, amount opts},
         schedule {start, end, repeat, days}, priority (0-9), pinned,
         approved_by, approved_at, reject_reason, version
```

- The style is the spec the Design and Message-lines tabs already use. The preview is therefore exactly what the
  sign will show (the firmware-checked renderer), and the bytes come from the existing encoder
  (`core.encode_markup`, `signproto.rs485_markup_frame`).
- Each edit makes a new version. An approval names the version it approved.

## 4. Getting messages onto the sign

- A scheduler in the studio keeps a rotation: the approved messages that are inside their schedule, ordered by
  priority, then age. Each is shown for its own scroll time, which the emulator already measures, and then the next
  one is sent.
- Send live (Moderator+) puts one message in front once, then the rotation resumes.
- One sender owns the serial port. All sends go through one queue, so two people cannot interleave frames.
- The rotation only sends messages that pass the warnings the studio already gives:
  - the text fits the sign's RS-485 rule (`core.record_text_scrolls`);
  - no hold that never ends;
  - under the record's length limit;
  - only fonts that are on the sign.
- When the sign or adapter is missing, the queue keeps running in the studio with a warning, and nothing is lost.

## 5. On the internet: where it runs and how it is protected (custom firmware first)

Where it runs (decided 2026-10-31): a portable kit that travels with the sign. The web interface is a separate
deployment so the sign can be taken places and set up there. The kit is the sign plus a Raspberry Pi CM4 box
(reComputer R1125-10) running the whole Docker Compose stack (portal, database, sender). It must work anywhere,
including with no internet.

```
 at a venue, no internet needed:
   phones/laptops --Wi-Fi--> the box (hotspot or venue LAN): portal (HTTPS), db, sender
                                   |  (the box is inside the marquee; its Ethernet is on the back panel)
                                   |  RS-485 and RS-232 ports
                                   v
                              the sign: RS-485 (content, and with our firmware replies),
                                        DB9 (our firmware's serial console)

 when internet is available (optional):
   the world --HTTPS--> tunnel (Cloudflare Tunnel / Tailscale Funnel) --> the same box
```

- The box is the server.
  - The box sits inside the marquee's enclosure, powered from its 5 V supply, and drives the sign over RS-485
    through its RS-485 port (docs/SIGN-LINK-PLAN.md). The sign has no network connection of its own; the
    box's Ethernet, on the back panel, is the only network port.
    - With the stock firmware, the box sends RS-485 records and message lines; the sign does not answer.
    - With our firmware, the sign also answers on RS-485 (status, acknowledgements) and accepts validated
      playlists and configs, and keeps its last good playlist if the box stops sending.
    - The server never pushes firmware; updates stay a deliberate, local act.
- Local only for now (decided), with cloud as a later option:
  - The kit needs no cloud service to run. Sign-in, posting, review, previews and the sign link all work offline.
  - When there is internet, the `tunnel` profile publishes the portal without opening ports, so remote people can
    post too. A VPS deployment remains possible with the same Compose file.
- Getting people onto it at a venue:
  - The box can run its own Wi-Fi hotspot or join the venue's network.
  - It announces itself by mDNS (`marquee.local`).
  - A Moderator can show a QR code (on a screen, or on the sign itself) for the portal's address and, if wanted,
    the hotspot's Wi-Fi details.
  - Invite links work the same way.
- HTTPS without internet: `marquee.local` cannot get a public certificate offline, so the kit uses Caddy's local
  certificate authority. Phones trust it after a one-time install, or the kit falls back to HTTP on the hotspot
  only. Every remote (tunnel) access is HTTPS.
- The CAPTCHA is optional because it needs the internet (Turnstile), and there is no email. Offline, anonymous
  posting relies on review being on and on per-device rate limits.
- Setting up at a new place is a checklist and needs no rebuild:
  1. power on; the box starts the stack (Compose `restart: unless-stopped`) and the hotspot;
  2. the sign finds the box (DHCP plus a fixed name); a "sign online" badge appears in the portal;
  3. switch the portal's site profile (event name, which levels may post, review on, quiet hours);
  4. show the QR code.
  - Site profiles (saved switch sets: "home", "party", "open event") make the change one click.
- Moving between places keeps the data, because the database travels on the box. The `backup` profile can sync
  dumps to a USB stick or, when online, to remote storage.
- It runs on Docker Compose for the entire setup (decided), on arm64 (Pi) and amd64. See §7a.

### Accounts and sign-in (Django; decided 2026-10-31)

- Django's auth, with a custom user model from day one (`AUTH_USER_MODEL`) that has a `level` foreign key.
  - Passwords are hashed with Argon2 (`argon2-cffi`, Django's recommended hasher), with PBKDF2 as fallback.
  - Django's password validators set the minimum length and refuse common passwords.
- Sign-in, sign-up and password reset come from django-allauth.
  - Local accounts only by default (decided 2026-10-31).
  - Cloud providers are an option an Admin turns on: Google, Microsoft, GitHub or any OIDC provider. Each is an
    allauth provider app, configured with its client ID and secret in the Django admin; none is installed or shown
    until then.
  - A provider sign-in still makes (or links to) a local account, which gets the level for new accounts. A provider
    can be turned off again without losing those accounts, because they can set a local password.
- No email (decided 2026-10-31; out of scope for now).
  - Sign-up is open, invite-only (single-use links or QR codes made by Moderators/Admins) or closed. This is a
    setting in the database, read by an allauth adapter. Accounts are identified by user name.
  - With open sign-up, new Members start with review on, and rate limits apply.
  - Password reset: a Moderator or Admin makes a single-use reset link or code, shown on screen or as a QR code.
  - Review notices appear in the portal (a badge and a live queue), with optional browser push notifications.
- Two-factor (TOTP) for Moderators and Admins is required once the portal is on the internet (allauth's MFA, or
  django-otp).

### Security (needed once it is on the internet)

- HTTPS only, via the tunnel or Caddy; HSTS (`SECURE_HSTS_SECONDS`, `SECURE_SSL_REDIRECT`,
  `SESSION_COOKIE_SECURE`, `CSRF_COOKIE_SECURE`; `manage.py check --deploy` passes).
- Sessions and CSRF: Django's session framework (database-backed) and CSRF middleware on every form and API
  write.
- Brute-force protection: sign-in throttling per account and per IP address, with lockout and back-off
  (django-axes); rate limits on posting (django-ratelimit, keyed by user and by IP).
- Access control: every endpoint checks the level on the server. Hiding buttons in the UI is only a convenience.
- Untrusted input: post text and names are shown escaped in the web pages. The text sent to the sign goes
  only through the markup encoder, so no raw bytes reach it.
- Uploads: none from the public part.
- Audit log (append-only): who did what and when (post, edit, approve, reject, remove, send live, clear,
  level change, sign-in, failed sign-in), with the IP address for anonymous posts.
- Abuse handling: a word block-list, rate limits, the "review everything" and "posting closed" switches, and
  Moderators can ban an account or an IP address.

## 6. Content policy (per level, Admin-editable)

- Allowed fonts, colours, effects and emoji sets.
  - Anonymous: plain text only. Members get the shareable fonts and the sign's native emoji by default.
  - The emoji pack's art is not redistributed (THIRD-PARTY.md): the portal shows only the native emoji set built
    from it.
- Markup limits for Members: plain text plus colour and a few entry/exit styles; Trusted get more. No raw ESC bytes
  and no includes of other lines (`ESC 0x80`).
- Odometer amounts (`core.amount_format`) and protocol options are Moderator+ only.
- Flashing: Anonymous and Members get no flash. Trusted get at most a cycle of 334 ms or more (at most 3 flashes a
  second, WCAG 2.3.1). The sign's Flash On/Off values are milliseconds (25 to 6375; `core.flash_warnings`).
  Animations and GIFs that flash fast or switch to saturated red are checked before they enter the rotation.
- Sound and relays stay out of messages: the firmware ties them to the jackpot-hit path, and whether a studio
  send can reach them is not confirmed.

Ideas for what to build with this (shoutouts, countdowns, birthday mode, polls, Home Assistant alerts, ...) are in
`docs/MARQUEE-IDEAS.md`, with a top 10.

## 7. What gets built (Django)

`portal/` is a public Django project (Python 3, Django LTS).

| App | Holds |
|---|---|
| `accounts` | the custom user, `Level` (the switches of §2, one row per level), invites, bans |
| `posts` | `Post` and `PostVersion` (text and style spec), schedule, priority, pin, status |
| `review` | the review queue, approve/reject with reason (approval names the version) |
| `rotation` | what plays next, send live, clear the sign, `SiteSettings` (posting open, review everything, sign-up mode) |
| `signlink` | the sign's status and link settings, the RS-485 sender (stock path and sign-link frames) |
| `audit` | the append-only `AuditEvent` (written by signals and views; no update/delete in the admin) |

- Database: PostgreSQL only (decided), on every host and in the tests, with Django migrations and `psycopg` 3.
- Front end: React (decided), in `frontend/`. It is a Vite + React + TypeScript single-page app that talks
  only to the Django REST API.
  - Pages: sign-in/up, post, my posts, now on the sign, the review queue (Moderator+), levels and users
    (Admin; the Django admin stays as the fallback).
  - Data: TanStack Query for API data; React Router.
  - UI: shadcn/ui exclusively (decided), meaning Radix primitives with Tailwind CSS. The components are added with
    the shadcn CLI into `frontend/packages/ui/`, vendored (§7b), and owned by the project (MIT, listed in
    THIRD-PARTY.md).
    - Forms (post, sign-up, level switches): shadcn `Form` with react-hook-form and zod.
    - Review queue, users, audit log: shadcn `DataTable` (TanStack Table) with filters, sorting and row actions.
    - Also `Dialog` / `AlertDialog` (approve, reject with a reason, "clear the sign"), `Switch` (level switches,
      "posting open", "review everything"), `Tabs`, `Badge` (status, level), `Sonner` toasts, `lucide-react`
      icons.
    - Light and dark themes through shadcn's CSS variables. The sign preview keeps its own dark LED look in both.
    - Radix gives keyboard and screen-reader support. It is tested with Playwright's accessibility checks
      (axe-core).
    - What "exclusively" means:
      - every control, layout and feedback element is a shadcn/ui component, or composed only from them and
        shadcn's theme tokens;
      - no other component library (MUI, Chakra, Ant, Mantine, Bootstrap, headless libraries used directly, ...);
      - no hand-made buttons, inputs, dialogs, menus or tables where shadcn has one.
    - The one exception is the sign preview canvas (`<SignPlayer>`), since shadcn has nothing like it. It sits
      inside shadcn components (`Card`, `Slider` for seeking, `Toggle` for LED dots, `Button`), and its controls
      are shadcn.
    - Enforced in CI:
      - ESLint `no-restricted-imports` blocks other UI libraries, and Radix imported outside
        `frontend/packages/ui/`;
      - a check fails on raw `<button>`, `<input>`, `<select>` and `<dialog>` outside `frontend/packages/ui/`.
  - Auth in the SPA: allauth's headless API (made for SPAs), with session cookies and CSRF on the same
    origin (Caddy serves the SPA and proxies `/api`, `/_allauth` and `/admin` to Django). No JWTs in browser
    storage.
  - Previews are rendered in the browser by `@marquee/engine`, which the fixtures hold identical to the server's
    Python. A React `<SignPlayer>` component (a port of the studio's canvas player, with LED dots and the smooth
    afterglow) plays them, so a post previews exactly as the sign will show it.
  - Types: the TypeScript API client is generated from the DRF OpenAPI schema (drf-spectacular), so the front
    end and the API cannot drift apart unnoticed.
  - Checks: ESLint and `tsc --noEmit` in CI. The Content-Security-Policy is strict, since the built app needs no
    inline scripts.
- The Django admin (Admin only, behind 2FA, at a non-default path) manages levels and their switches, users,
  invites and site settings, and gives a read-only view of the audit log.
- API: Django REST Framework, with one permission class per level switch, so §2 lives in one place.
  - Endpoints: posts, review, rotation, and (Admin) levels and users.
  - The sign does not use the HTTP API. The `sender` talks to it over RS-485: the stock path for the stock
    firmware, sign-link frames for ours (docs/SIGN-LINK-PLAN.md).
- Background work: no Celery (decided). A management command, `manage.py run_sender`, runs as its own Compose
  service. It plays the rotation over RS-485 for the stock firmware and expires posts. Scheduling uses the database
  (`SELECT ... FOR UPDATE SKIP LOCKED` on due posts), so no Redis or broker is needed.
- Anonymous posting: Cloudflare Turnstile, verified on the server. The anonymous rate limit is keyed by IP
  address; the address is kept in the audit log for a limited time, then deleted.
- Umami Marquee Studio lives in the front end, standalone and inside the portal (§7b).
- Custom firmware side: docs/SIGN-LINK-PLAN.md (sign-link frames on RS-485, validation, the playlist cache, the
  serial debugger).

## 7a. Docker Compose (decided: the entire setup)

One `compose.yaml` at `portal/` (or the repo root) brings everything up with `docker compose up -d`.

| Service | Image / build | Role | Exposed |
|---|---|---|---|
| `db` | `postgres` (pinned major) | the database; volume `pgdata`; healthcheck `pg_isready` | internal network only |
| `web` | built from `portal/Dockerfile` (python-slim, gunicorn) | Django: the API, allauth headless, the admin; runs `migrate` and `collectstatic` on start | to `proxy` only |
| `sender` | the `web` image, `manage.py run_sender` | the rotation; the RS-485 transport (stock path and sign-link frames) | internal |
| `proxy` | built from `frontend/Dockerfile`: a Node stage runs `vite build`, then a `caddy` stage serves the built SPA | HTTPS (automatic certificates), the React app, `/api` `/_allauth` `/admin` proxied to `web`, security headers (CSP), request size limits | 80/443 (VPS) |
| `tunnel` (profile `tunnel`) | `cloudflare/cloudflared` | HTTPS without open ports, for a Pi at home; replaces `proxy`'s public ports | outbound only |
| `backup` (profile `backup`) | `postgres` image with a small loop | nightly `pg_dump`, keeps N days, to a volume or bind mount | none |

- Profiles keep one file for every setup:

  | Setup | Command |
  |---|---|
  | VPS | `docker compose up -d` |
  | Pi at home | `--profile tunnel` |
  | the kit (the box's port), or an adapter on this machine | `--profile rs485` (adds the device to `sender`) |
- The RS-485 device (the box's port, or a USB adapter on a desktop):
  - It is passed to `sender` only (`devices: ["/dev/serial/by-id/...:/dev/rs485"]`), with the container user in the
    `dialout` group.
  - No other container sees it.
  - No container runs privileged.
- Secrets (Django `SECRET_KEY`, database password, Turnstile keys, provider secrets) come from an
  `.env` file or Docker secrets.
  - They are never committed: `.env` is git-ignored, and `.env.example` is committed.
- Hardening:
  - The containers run as non-root, with `read_only: true` where possible and dropped capabilities.
  - `db` is on an internal network with no published port.
  - Healthchecks gate start-up order.
  - Images are pinned by version (and by digest for releases).
  - `restart: unless-stopped`.
- Volumes: `pgdata`, `static`, `media` (no public uploads), `caddy_data`, `backups`.
- Architectures: images build for `linux/amd64` and `linux/arm64`, so the same file runs on a VPS and on a Pi 4/5.
- Development: `compose.override.yaml` runs Django's dev server and the Vite dev server (hot reload,
  proxying `/api` to Django) with the sources mounted, and a published Postgres port for local tools.
- Tests run in Compose too:
  - back end, against PostgreSQL: `docker compose run --rm web python manage.py test`;
  - front end: Vitest + React Testing Library (components, the permission-dependent UI);
  - end to end: Playwright, against the whole stack (sign up, post, review, approve, on the sign).

  `./check.sh` calls them when Docker is available.

## 7b. Umami Marquee Studio: standalone and inside the portal, living in the front end (decided 2026-10-31)

The studio lives in the front end. It is one React + shadcn package with its engine (rendering, markup, fonts,
emoji, amounts, flash rules, content packs) in TypeScript, so it runs entirely in the browser:
- Standalone: a static web app with no server, no database and no Docker. Open it from a local folder or any
  static host.
- Embedded: the same package mounted in the portal at `/studio`, for Moderators and Admins.

```
             studio-ui  (React + shadcn, engine in TypeScript)
               <Studio storage=... transport=... capabilities=...>
                    |                                   |
     apps/studio (static, standalone)          apps/portal  (/studio route)
       storage: browser (OPFS/IndexedDB)         storage: the portal API (Postgres + media)
                or a folder (File System Access)
       transport: Web Serial -> USB RS-485       transport: the portal's rotation -> sender / sign link
```

What the browser does:
- Engine: `@marquee/engine`, a TypeScript port of the studio's Python core:
  - the panel renderer (text items, fonts, effects, outlines, afterglow preview);
  - message markup encode/decode and its warnings, `amount_format`, the flash rules;
  - FLC decode and GIF import, BDF/TTF font import, emoji fonts, content packs (LFS).
  - Pure functions on typed arrays; heavy work runs in a Web Worker so the UI stays smooth.
- Storage is an interface:
  - standalone: the browser's private storage (OPFS / IndexedDB), or a folder the user picks (File System Access
    API, Chromium), so content can live next to the sign's files;
  - embedded: the portal's API.
- Transport is an interface:
  - standalone: Web Serial to the USB RS-485 adapter (Chrome / Edge only; elsewhere the UI says so), or
    "download the frame";
  - embedded: the portal's rotation (send live goes to the sender or the sign link). The browser never talks to
    the serial port in the portal.
- Capabilities (what the user may do) come from the host: everything when standalone, and the user's level when
  embedded. The UI hides only what the server refuses anyway.

TypeScript for the front end, Python for the back end (decided 2026-10-31):
- TypeScript is only the front end: `@marquee/engine` (rendering, markup, fonts, emoji, amounts, flash rules,
  packs) and the React UI. Umami Marquee Studio uses it standalone and inside Marquee Portal; the portal's post composer
  uses it for previews.
- The portal's back end is Django, in Python. It never trusts the browser: it re-checks and re-encodes every
  post with the Python package `marquee_studio`, which is today's firmware-checked `studio/core.py`,
  `native_emoji.py`, `font_import.py`, the emulator and `signproto.py`, packaged. The sender uses it to produce the
  exact bytes or frames for the sign. Posts are stored as text plus a style spec, never as client-made bytes.
- Golden fixtures hold the two implementations identical.
  - The Python package is checked against the real firmware (the Unicorn harness, desktop tooling).
  - It generates the fixtures: inputs plus the exact frames, bytes and warnings.
  - The TypeScript engine must match them pixel for pixel and byte for byte (Vitest, in CI).
  - The chain is firmware → Python → fixtures → TypeScript; a difference anywhere fails the build.
- Desktop tools are fine for development work: Python, Unicorn, clang and Ghidra stay as test and analysis
  tooling.
- Running Python in the browser with Pyodide was considered and rejected: it is a 20+ MB download and takes
  seconds to start.

Front-end layout (npm workspaces; pnpm is not needed):

```
frontend/packages/ui/          shadcn/ui components, VENDORED (see below)
frontend/packages/engine/      @marquee/engine: the TypeScript port + golden-fixture tests
frontend/packages/studio-ui/   <Studio>: the tabs as routes (Design, Message lines, Effects, Fonts, Emoji,
                               Animations, Content pack, Settings)
frontend/packages/sign-player/ <SignPlayer> (also used by the portal's post composer)
frontend/apps/studio/          standalone static app
frontend/apps/portal/           the portal SPA
studio/marquee_studio/         Python package: the firmware-checked reference, used by the portal's back end
```

shadcn components are vendored (decided):
- They are copied into `frontend/packages/ui/` once with the shadcn CLI, committed, and owned by the project.
- Nothing is fetched from the shadcn registry at build or run time.
- Their Radix and Tailwind dependencies are pinned in the lockfile.
- Updating one is a deliberate CLI run, reviewed as a diff.
- Both apps import only from `@marquee/ui`. THIRD-PARTY.md lists the vendored components (MIT).

Migration, without breaking today's studio:
1. Package the Python modules as `marquee_studio` (today's Flask studio and all tests keep working) and add the
   golden-fixture generator.
2. Port the engine to TypeScript module by module (markup and amounts first, then the renderer, effects, fonts,
   FLC/GIF, packs), each passing its fixtures.
3. Build `studio-ui` tab by tab. The old Flask studio stays until each tab has a React replacement that
   Playwright shows matches.
4. Standalone: the static app with browser storage and Web Serial. The Flask studio then retires.
5. Embedded: mount `studio-ui` in the portal with portal storage and transport.

## 8. Testing

The portal follows the same habit as the rest of the project (`./check.sh`):
- Permissions: for every endpoint, every level and every switch setting (review on/off, posting closed, sign-ups
  open/invite/closed, Anonymous on/off), allowed and refused. The switch combinations become test data.
- Message lifecycle: draft → pending → approved / rejected; an edit after approval goes back to pending;
  schedule expiry.
- Rendering: a stored message renders to the same frames as the Design tab, and the frame the scheduler sends
  decodes back (`decode_markup`) to the approved version.
- Security: no session → 401, wrong role → 403, missing CSRF token → refused, rate limit, password hashing.
- On the sign: hardware tests stay manual. The first live send of a queued message is done with someone present
  at the sign (BACKLOG item 4).

## 9. Steps

1. Levels, accounts and sessions: the Compose setup (db, web, proxy), the React skeleton (sign-in/up against
   allauth headless), the Django project, custom user and `Level` models, allauth sign-in, invite links, the
   admin, DRF permission classes, and tests for the whole permission matrix (Django's test runner, called from
   `./check.sh`).
2. Posts and review: posting with the React `<SignPlayer>` preview, the review queue, the audit log. No sending
   yet.
3. The sender and rotation: schedules, priorities, send live, clear the sign. RS-485 comes first (it works on
   today's firmware), then sign-link frames once our firmware answers them.
4. Content policy per level, rate limits, block-list, and Anonymous posting (off by default; review always on for
   it, rate-limited per device; the CAPTCHA waits for remote access).
5. Deployment: the box's image (CM4, eMMC) and hotspot set-up, the UPS shutdown script, the `backup` and `rs485`
   profiles, two-factor for staff, multi-arch images, and a deployment guide for a VPS and for a Pi.
6. Optional: OIDC sign-in, browser push notifications, the ideas in `docs/MARQUEE-IDEAS.md`.

## 10. Decisions and open questions

Decided (2026-10-31):
- one sign (no per-sign fields);
- local access only for now: the portal is reached on the Pi's hotspot or the venue LAN (`marquee.local`), with
  no domain, no tunnel and no remote access yet (the `tunnel` profile and Turnstile CAPTCHA are deferred);
- no email (out of scope for now): invites/QR codes, staff-made reset links, in-app review notices;
- the box is a Seeed reComputer R1125-10 inside the marquee (Raspberry Pi CM4, 4 GB RAM, 32 GB eMMC, Wi-Fi):
  its isolated RS-485 and RS-232 ports to the sign (RS-485 bus and DB9), its Ethernet on a panel-mount RJ45 in the
  back for a wired venue network, Wi-Fi for the hotspot, power from the enclosure; arm64 images. The marquee may be
  switched off at the wall at any time, so the SuperCAP UPS module holds the box up long enough to shut down
  cleanly, the root filesystem is read-only (an overlay), Postgres keeps its data on a separate partition with
  fsync on, and the `backup` profile copies a nightly dump to a USB stick;
- the portal's back end is Django (DRF for the API, allauth for accounts);
- Umami Marquee Studio lives in the front end (React, shadcn, TypeScript engine), standalone and inside Marquee
  Portal. TypeScript is front end only: the portal's Django back end re-checks and encodes with the Python
  `marquee_studio` package, and golden fixtures keep the two engines identical;
- shadcn components are vendored into `frontend/packages/ui` (committed, no registry fetches);
- PostgreSQL only; no Celery (a management-command worker); Docker Compose for the entire setup;
- React front end (Vite, TypeScript) on the Django REST API, with session-cookie auth via allauth headless;
- shadcn/ui exclusively (Radix + Tailwind), with react-hook-form + zod forms and DataTable views; the sign
  preview canvas is the only custom element, wrapped in shadcn components; enforced by lint;
- sign-in: local accounts, with cloud providers (Google, Microsoft, GitHub, OIDC) as an option the Admin enables;
- levels from Anonymous to Admin, with review on/off per level;
- accounts in most cases;
- a portable kit: the sign plus a box running the Compose stack, working offline at any venue. Internet (a
  tunnel) is optional. The box is inside the marquee and drives the sign over RS-485, with either firmware;
- desktop tools are fine for development and testing (Python, Unicorn, clang, Ghidra).

Still open: nothing blocking. Deferred: remote access (tunnel, a domain), the CAPTCHA, OIDC providers,
email, more than one sign.
