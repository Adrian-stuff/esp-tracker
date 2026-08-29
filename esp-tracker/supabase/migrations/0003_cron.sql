-- Scheduled work.
--
-- pg_cron drives the two jobs that used to be background tasks in a
-- long-running Python process. Enable the extensions first (Supabase dashboard:
-- Database → Extensions, or run these).

create extension if not exists pg_cron  with schema extensions;
create extension if not exists pg_net   with schema extensions;

-- ---------------------------------------------------------------------------
-- 1. Drain the escalation queue, every minute.
--
-- Set these once per project (Dashboard → Settings → Vault, or via SQL):
--   select vault.create_secret('https://<ref>.supabase.co', 'project_url');
--   select vault.create_secret('<service-role-key>',        'service_role_key');
-- ---------------------------------------------------------------------------
select cron.schedule(
    'dispatch-alerts',
    '* * * * *',
    $$
    select net.http_post(
        url     := (select decrypted_secret from vault.decrypted_secrets where name = 'project_url')
                   || '/functions/v1/dispatch',
        headers := jsonb_build_object(
            'Content-Type',  'application/json',
            'Authorization', 'Bearer ' || (select decrypted_secret from vault.decrypted_secrets
                                           where name = 'service_role_key')),
        body    := '{}'::jsonb
    );
    $$
);

-- ---------------------------------------------------------------------------
-- 2. Retention, nightly at 03:15.
--
-- This is not housekeeping, it is liability control. Wi-Fi scans go first and
-- fastest: an access-point trail is more revealing than the coordinate history
-- next to it. Run it, and check that it is actually running.
-- ---------------------------------------------------------------------------
create or replace function prune_old_data()
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    delete from wifi_scans    where recorded_at < now() - interval '7 days';
    delete from locations     where recorded_at < now() - interval '90 days';
    delete from device_health where reported_at < now() - interval '30 days';
    delete from access_log    where at          < now() - interval '180 days';
    delete from alert_queue   where status <> 'pending'
                              and due_at < now() - interval '30 days';
    -- sos_events are kept indefinitely, on purpose.
end;
$$;

select cron.schedule('prune-old-data', '15 3 * * *', $$ select prune_old_data(); $$);

-- Useful while setting up:
--   select * from cron.job;
--   select * from cron.job_run_details order by start_time desc limit 20;
