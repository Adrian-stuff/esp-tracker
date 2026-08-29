# web/ — Vercel dashboard

Static files. No build step, no framework. Talks to Supabase directly: PostgREST for queries,
Realtime for live updates, Supabase Auth for sign-in.

```bash
cd web && vercel --prod
```

Set your project URL and anon key in `config.js` first.

## Why there is no API layer here

Every query in `app.js` goes straight to Postgres, and RLS decides what comes back. `select * from
locations` returns only the devices this parent was granted — the policy does the filtering, not
application code that could forget to.

Two things still need a server, and both are Supabase Edge Functions:

- **`ingest`** — devices are not Auth users and need a service-role write path.
- **`locate`** — sending an SMS needs a provider key, which must never reach the browser.

## Before real data

`config.js` holds the anon key, which is *meant* to be public — but only because RLS is on. Run
`supabase/migrations/0002_rls.sql` and confirm every table shows "RLS enabled" in the Table Editor.
Without it this page is an open door to a child's live location.
