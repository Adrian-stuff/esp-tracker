# Supabase + Vercel deployment

Postgres, auth, realtime and the scheduled jobs live in Supabase. Vercel serves the dashboard as
static files. There is no always-on server.

## What changed, and why

Vercel functions are killed the moment they return a response. Three things in `server/` depended
on a long-running process, and each needed a different answer:

| Was | Now | Why it is better, not just different |
|---|---|---|
| asyncio escalation ladder sleeping up to 300 s | `alert_queue` rows + `pg_cron` every minute | The asyncio version lost every pending escalation on restart. Rows survive |
| Hand-rolled WebSocket hub | Supabase Realtime | RLS applies to the stream, so a parent is only pushed their own devices |
| Custom users/sessions + argon2 + TOTP | Supabase Auth | RLS policies get `auth.uid()` for free; MFA is built in |

`server/` still works and is the reference implementation — keep it for local development, or
deploy it to Fly/Railway instead of this stack if you would rather keep one Python service.

## Setup

**1. Create the project** at supabase.com. Region **Southeast Asia (Singapore)** — closest to PH.

**2. Run the migrations** in order, in the SQL editor:

```bash
supabase link --project-ref YOUR-REF
supabase db push
```

Or paste `migrations/0001_schema.sql`, `0002_rls.sql`, `0003_cron.sql` in that order.

**3. Verify RLS is on before putting real data in.** Table Editor → every table must show
*RLS enabled*. This is not a formality; see the warning below.

**4. Set the function secrets** (never in the repo, never in `web/`):

```bash
supabase secrets set SMS_PROVIDER=semaphore \
  SEMAPHORE_API_KEY=... SEMAPHORE_SENDER_NAME=... SMS_CMD_SECRET=...
```

`SMS_PROVIDER=console` logs instead of sending, so you can exercise the whole ladder for free.

**5. Deploy the functions:**

```bash
supabase functions deploy ingest  --no-verify-jwt   # devices use their own bearer token
supabase functions deploy dispatch --no-verify-jwt  # called by pg_cron with the service role
supabase functions deploy locate                    # parent-called: JWT verified
```

`ingest` needs `--no-verify-jwt` because devices are **not** Supabase Auth users — they present a
per-device token whose sha256 is in `devices.token_hash`, checked inside the function.

**6. Store the cron secrets in Vault** (Dashboard → Settings → Vault):

```sql
select vault.create_secret('https://YOUR-REF.supabase.co', 'project_url');
select vault.create_secret('YOUR-SERVICE-ROLE-KEY',        'service_role_key');
```

Then confirm the jobs registered: `select * from cron.job;`

**7. Deploy the dashboard to Vercel:**

```bash
cd web && vercel --prod
```

Set `SUPABASE_URL` and `SUPABASE_ANON_KEY` in `web/config.js` first. Both are meant to be public.

**8. Register a parent and a device:**

```sql
-- After signing up through the dashboard's sign-in form:
insert into devices (id, kind, name, child_name, token_hash, msisdn)
values ('tracker-01','tracker','Tracker 01','Ana',
        encode(digest('PASTE-A-RANDOM-TOKEN','sha256'),'hex'), '+639XXXXXXXXX');

insert into device_access (user_id, device_id, escalation_order, phone)
values ((select id from auth.users where email='parent@example.com'),
        'tracker-01', 1, '+639XXXXXXXXX');
```

Put that random token in `tracker/include/config.h` as `DEVICE_TOKEN`, and point `API_HOST` at
`YOUR-REF.supabase.co` with path `/functions/v1/ingest`.

---

## ⚠ RLS is the whole security model

Supabase exposes every table over PostgREST using the anon key, **which ships in your frontend
bundle**. A table without RLS enabled is readable by anyone who opens devtools.

For a live database of a child's real-time location, that is not a misconfiguration — it is the
worst outcome this project has. `0002_rls.sql` enables RLS on all fourteen tables and adds
policies routed through `device_access`. Three tables (`alert_queue`, `access_log`,
`integration_health`) deliberately get **no policies at all**: RLS on with nothing matching means
they are invisible to everyone except the service role.

**The service role key bypasses RLS entirely.** It belongs in `supabase secrets` and Vercel
environment variables. It must never appear in `web/`, in git, or in any client.

## Escalation timing

`pg_cron` granularity is 60 s, so a rung due at t+60 fires somewhere in t+60..t+120.

Rung 1 is **not** queued — it is sent synchronously inside the ingest request, so the first alert
is never delayed by a scheduler. Only the secondary-contact SMS and the two voice calls go through
the queue, where ±60 s does not matter.

If you later need to-the-second timing, replace the cron job with delayed callbacks (QStash and
similar schedule precisely) and keep the same `alert_queue` table.

## Cost

Both free tiers cover this comfortably at one or two devices. Watch these as it grows:

- **Supabase free**: 500 MB database, 2 GB egress, 500K Edge Function calls/month. A tracker
  reporting every 2 min is ~22K ingest calls/month.
- **`pg_cron` every minute** is 43K dispatch calls/month on its own. If that becomes the dominant
  cost, drop the drain to every 2 minutes and accept coarser rungs, or move to delayed callbacks.
- **Vercel Hobby** is fine for static hosting. Note Hobby cron is once-per-day, which is why the
  scheduling lives in Supabase rather than Vercel.
