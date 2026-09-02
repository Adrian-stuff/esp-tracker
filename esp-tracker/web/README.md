# web/ — Vercel dashboard

Static files. No build step, no framework. Talks to Supabase directly: PostgREST for queries,
Realtime for live updates, Supabase Auth for sign-in.

```bash
cd web && vercel --prod
```

Set your project URL and anon key in `config.js` first.

## Git-based deploys — Root Directory MUST be `esp-tracker/web`

This directory's actual position in the connected Git repository is **not** the repo root. Verify with:

```bash
git rev-parse --show-toplevel   # -> .../possible-client  (NOT .../esp-tracker)
```

The repo (`github.com/Adrian-stuff/esp-tracker`) is cloned into a `possible-client` parent
directory whose only content is a top-level `esp-tracker/` folder — so relative to the Git repo
root, this dashboard lives at `esp-tracker/web`, not `web`.

**Action needed in the Vercel dashboard** (not fixable from `vercel.json` — Root Directory is a
project setting, not a file-based config key): Project → Settings → General → **Root Directory**
must be set to `esp-tracker/web`. `vercel project inspect web` was seen reporting `.` — if that is
still the case, a GitHub-triggered build looks for files at the true repo root and will not find
this dashboard.

This does NOT affect manual CLI deploys (`vercel deploy --prod` run from inside `web/`) — those
package whatever directory you run the command from, ignoring the Root Directory project setting
entirely. That is why CLI deploys have worked throughout this project even if the Git-triggered
path is misconfigured.

**Also check Deployment Protection.** A GitHub push once produced a Production deployment that
redirected every request to `vercel.com/sso-api` (Vercel's own login wall) instead of serving this
site — confirmed via the `Location` response header, not guessed. That happened on a
git-integration deployment, not a CLI one. If it recurs, re-point the alias to the last good CLI
deployment (`vercel alias set <good-deployment-url> <alias-domain>`) as an immediate fix, then
check Project → Settings → **Deployment Protection** and make sure "Vercel Authentication" is not
applied to the Production environment.

`vercel.json` now pins `"framework": null` and `"buildCommand": null` explicitly, so a Git-triggered
build cannot guess a framework or run an unwanted install/build step — it always serves these files
as-is, matching exactly what a CLI deploy already does.

## Pages

- **`index.html`** — landing page. Just asks which dashboard you want; no auth, no queries.
- **`tracker.html`** / **`tracker.js`** — parent view: live map, battery/SOS, named places.
- **`attendance.html`** / **`attendance.js`** — school-gate view: recent taps, daily in/out,
  and the status of each gate scanner (online/offline, last health report, queue depth, and a
  rename control via the `rename_device` RPC — see `supabase/migrations/0007_rename_device.sql`).
- **`common.js`** — the one Supabase client and the one sign-in/sign-up flow, shared by both
  dashboards, so there is exactly one auth implementation to get right.

Both dashboards are gated the same way: `device_access` rows decide what a signed-in account can
see, regardless of which page it's on. A school account granted access to a scanner but no tracker
sees an empty parent view and a populated attendance view, and vice versa.

## Scanner "configuration" — what this page can and can't do

The attendance page can **rename** a scanner (cosmetic, dashboard-only) and show its last reported
**status** (online/offline, firmware, battery, signal, offline-queue depth, uptime, last reset
reason) — all of that already flows through `ingest`'s `health` events into `devices` and
`device_health`.

It CANNOT remotely change WiFi credentials, the API server, or the SMS numbers — the firmware has
no command channel for that. Those are set locally, once, at the scanner's own captive portal
(`Tracker-Scanner` AP, see `scanner/src/net.cpp` / `scanner/src/settings.cpp`). Wiring up a real
remote-config channel would mean the scanner polling for pending config changes (similar to how it
already polls `roster`), which is a firmware change, not a dashboard one.

## Why there is no API layer here

Every query in `tracker.js` / `attendance.js` goes straight to Postgres, and RLS decides what comes
back. `select * from locations` returns only the devices this parent was granted — the policy does
the filtering, not application code that could forget to.

Two things still need a server, and both are Supabase Edge Functions:

- **`ingest`** — devices are not Auth users and need a service-role write path.
- **`locate`** — sending an SMS needs a provider key, which must never reach the browser.

## Before real data

`config.js` holds the anon key, which is *meant* to be public — but only because RLS is on. Run
`supabase/migrations/0002_rls.sql` and confirm every table shows "RLS enabled" in the Table Editor.
Without it this page is an open door to a child's live location.
