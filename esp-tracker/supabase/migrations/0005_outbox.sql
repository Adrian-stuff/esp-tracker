-- Device-relayed SMS.
--
-- Both modems are paid for already, and on a matching PH network a device-sent
-- text is effectively free where a provider charges per message. Routine
-- notifications are offered to the devices first; the provider is the guarantee.
--
-- SOS NEVER ENTERS THIS TABLE. An emergency must not wait for a device to poll,
-- claim and confirm — the escalation ladder keeps using the provider directly.
--
-- child_device_id encodes the privacy rule: a scanner may relay anything, but a
-- TRACKER may only relay messages about its own child, whose parent number it
-- already holds for SOS. A general relay would push other families' numbers
-- onto a child's device.
create table if not exists outbox (
    id              bigserial primary key,
    to_number       text not null,
    body            text not null,
    child_device_id text references devices(id) on delete set null,
    status          text not null default 'pending'
                    check (status in ('pending','claimed','sent','provider_sent','failed','expired')),
    claimed_by      text references devices(id) on delete set null,
    lease_until     timestamptz,      -- stops two devices sending the same message
    fallback_after  timestamptz not null,
    attempts        int not null default 0,
    created_at      timestamptz not null default now(),
    sent_at         timestamptz,
    ref             text,
    error           text
);
create index if not exists ix_outbox_pending on outbox (status, fallback_after);

alter table outbox enable row level security;
-- No policies: only the service role touches this. It holds phone numbers.

-- The FastAPI build runs this as an asyncio loop; a serverless function cannot,
-- so here it is a scheduled job against the same table and the same rules.
select cron.schedule(
    'sweep-outbox',
    '* * * * *',
    $$
    select net.http_post(
        url     := (select decrypted_secret from vault.decrypted_secrets where name = 'project_url')
                   || '/functions/v1/sweep-outbox',
        headers := jsonb_build_object(
            'Content-Type',  'application/json',
            'Authorization', 'Bearer ' || (select decrypted_secret from vault.decrypted_secrets
                                           where name = 'service_role_key')),
        body    := '{}'::jsonb);
    $$
);
