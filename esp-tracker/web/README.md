# web/ — Vercel dashboard

Static files. No build step, no framework. Talks to Supabase directly: PostgREST for queries,
Realtime for live updates, Supabase Auth for sign-in.

```bash
cd web && vercel --prod
```

Set your project URL and anon key in `config.js` first.

## Deploys are CLI-only — the GitHub integration is deliberately DISCONNECTED

This project's Git repository layout is unusual: this directory is at `esp-tracker/web` relative
to the true repo root (`git rev-parse --show-toplevel` → `.../possible-client`, a parent directory
whose only content is a top-level `esp-tracker/` folder), while manual CLI deploys
(`vercel deploy --prod`, run from inside `web/`) package whatever directory you run them from,
ignoring Vercel's "Root Directory" project setting entirely.

That mismatch caused two separate, real production outages this project actually hit:

1. **Root Directory unset/`.`**: a GitHub-triggered build looked for files at the true repo root,
   found nothing deployable there, and the resulting broken deployment auto-aliased over a
   perfectly good CLI deployment — the live site 404'd on every page.
2. **Root Directory set to `esp-tracker/web`** (the "fix" for #1): CLI deploys run from inside
   `web/` then appended that same path AGAIN (`web/esp-tracker/web`, which doesn't exist), and a
   `vercel deploy` from the repo root with no project linked there instead created a **brand new,
   wrong Vercel project** and started uploading the entire ~270MB monorepo before being caught and
   deleted.

Root Directory cannot be made to satisfy both deploy paths at once given this repo's actual shape.
**The fix: the GitHub integration is unlinked from this Vercel project entirely** (`DELETE
/v9/projects/{id}/link` via the Vercel API — there's no CLI subcommand for it). A `git push` now
does nothing to Vercel, on purpose. The ONLY way this site deploys is:

```bash
cd web && vercel deploy --prod
```

**Do not re-connect the GitHub integration or set a Root Directory** without re-solving this
mismatch first — either fix would silently reintroduce one of the two outages above the next time
someone pushes to `main`.

Also worth knowing, independent of the above: a GitHub-triggered deployment once redirected every
request to `vercel.com/sso-api` (Vercel's own login wall) instead of serving this site — Deployment
Protection applied to Production. Moot while the integration stays disconnected, but check Project
→ Settings → **Deployment Protection** first if the integration is ever reconnected.

`vercel.json` pins `"framework": null` and `"buildCommand": null` explicitly, so even a one-off
manual re-deploy from the dashboard UI can't guess a framework or run an unwanted install/build
step — it always serves these files as-is, matching exactly what the CLI deploy does.

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
