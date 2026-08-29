-- Child tracker — Supabase / Postgres schema.
--
-- Ported from server/schema.sql (SQLite). Three structural changes, each forced
-- by moving to a serverless host:
--
--   1. ALERT_QUEUE replaces the in-process asyncio escalation ladder. A Vercel
--      function is killed the moment it returns its response, so a background
--      task that sleeps for 300 s cannot exist. Pending rungs become ROWS with
--      a due_at, drained by pg_cron every minute.
--      This is strictly better than what it replaces: the asyncio version lost
--      every pending escalation on restart. Rows survive.
--   2. Realtime replaces the hand-rolled WebSocket hub (see the publication at
--      the bottom). Vercel serverless functions cannot hold a socket open.
--   3. auth.users replaces the custom users/sessions tables, so RLS policies
--      get auth.uid() for free.
--
-- Timestamps are timestamptz, not epoch integers. Devices still SEND epoch
-- seconds; the ingest function converts with to_timestamp().

-- ============================================================ devices =======
create table if not exists devices (
    id            text primary key,                 -- 'tracker-01'
    kind          text not null check (kind in ('tracker','scanner')),
    name          text not null,
    child_name    text,
    token_hash    text not null,                    -- sha256 of the per-device bearer
    msisdn        text,                             -- for the "locate now" SMS downlink
    firmware      text,
    battery_pct   int,
    signal_csq    int,
    balance_pesos int,                              -- prepaid load: a silent killer
    last_seen_at  timestamptz,
    active        boolean not null default true,
    created_at    timestamptz not null default now()
);
create index if not exists ix_devices_token on devices (token_hash);

-- ============================================================= places =======
create table if not exists places (
    id            bigserial primary key,
    device_id     text not null references devices(id) on delete cascade,
    name          text not null,
    kind          text not null default 'other' check (kind in ('home','school','other')),
    bssid_set     jsonb,                            -- hashed BSSIDs, parent-selected
    ble_anchor_id text,
    lat           double precision,
    lon           double precision,
    radius_m      double precision default 40
);
create index if not exists ix_places_device on places (device_id);

-- Recent Wi-Fi scans so a parent can name a place from what the tracker sees.
-- BSSIDs are stored HASHED: an access-point trail is a finer-grained record of
-- where a child has been than the coordinates sitting next to it.
create table if not exists wifi_scans (
    id          bigserial primary key,
    device_id   text not null references devices(id) on delete cascade,
    recorded_at timestamptz not null,
    aps         jsonb not null,                     -- [{h, ssid, rssi}]
    place_id    bigint references places(id) on delete set null
);
create index if not exists ix_wifi_device_time on wifi_scans (device_id, recorded_at desc);

-- ========================================================== locations ======
create table if not exists locations (
    id          bigserial primary key,
    event_id    text unique,                        -- device-generated: retries are idempotent
    device_id   text not null references devices(id) on delete cascade,
    lat         double precision not null,
    lon         double precision not null,
    accuracy_m  double precision not null,
    source      text not null check (source in ('gnss','wifi','ble_anchor','cell','manual')),
    place_id    bigint references places(id) on delete set null,
    recorded_at timestamptz not null,               -- when it HAPPENED, on the device
    received_at timestamptz not null default now(), -- when the server got it
    battery_pct int,
    speed_mps   double precision,
    heading     double precision
);
create index if not exists ix_loc_device_time on locations (device_id, recorded_at desc);

-- ========================================================== sos_events =====
create table if not exists sos_events (
    id                bigserial primary key,
    event_id          text unique not null,
    device_id         text not null references devices(id) on delete cascade,
    triggered_at      timestamptz not null,
    received_at       timestamptz not null default now(),
    latency_ms        int,
    first_location_id bigint references locations(id),
    best_location_id  bigint references locations(id),
    -- The device texts the parent directly over 2G. When it reports that it
    -- did, the ladder skips its own t+0 SMS: one press must never be two texts.
    device_sms_sent   boolean not null default false,
    status            text not null default 'open'
                      check (status in ('open','acknowledged','resolved','false_alarm')),
    acknowledged_by   uuid references auth.users(id),
    acknowledged_at   timestamptz,
    resolved_at       timestamptz,
    is_drill          boolean not null default false,
    notes             text
);
create index if not exists ix_sos_open on sos_events (status, triggered_at desc);

-- ========================================================= alert_queue =====
-- The escalation ladder, as data rather than as a sleeping coroutine.
-- pg_cron calls the dispatch Edge Function every minute; it claims rows whose
-- due_at has passed and whose SOS is still 'open'.
--
-- NOTE ON TIMING: cron granularity is 60 s, so a rung scheduled for t+60 fires
-- somewhere in t+60..t+120. Acceptable for rungs 2 and up. Rung 1 (t+0) is NOT
-- queued — it is sent synchronously inside the ingest request, so the first
-- alert is never delayed by the scheduler.
create table if not exists alert_queue (
    id           bigserial primary key,
    sos_event_id bigint not null references sos_events(id) on delete cascade,
    channel      text not null check (channel in ('push','sms','sms2','voice','voice2')),
    due_at       timestamptz not null,
    status       text not null default 'pending'
                 check (status in ('pending','sent','skipped','cancelled','failed')),
    attempts     int not null default 0,
    claimed_at   timestamptz,
    error        text
);
create index if not exists ix_queue_due on alert_queue (status, due_at)
    where status = 'pending';

-- Audit trail: proves what actually reached whom.
create table if not exists alerts (
    id              bigserial primary key,
    sos_event_id    bigint not null references sos_events(id) on delete cascade,
    channel         text not null check (channel in ('push','sms','voice','email','realtime')),
    recipient       text not null,
    sent_at         timestamptz not null default now(),
    provider_msg_id text,
    delivery_status text not null default 'sent',
    error           text
);

-- ========================================================= attendance ======
create table if not exists cards (
    card_uid   text primary key,
    device_id  text references devices(id) on delete set null,
    child_name text not null,
    active     boolean not null default true
);

create table if not exists attendance (
    id              bigserial primary key,
    event_id        text unique,
    scanner_id      text not null references devices(id) on delete cascade,
    card_uid        text not null,
    child_device_id text references devices(id) on delete set null,
    direction       text not null default 'in' check (direction in ('in','out')),
    recorded_at     timestamptz not null,
    received_at     timestamptz not null default now()
);
create index if not exists ix_att_time on attendance (recorded_at desc);

-- ========================================================== geofences ======
create table if not exists geofences (
    id        bigserial primary key,
    device_id text not null references devices(id) on delete cascade,
    name      text not null,
    lat       double precision not null,
    lon       double precision not null,
    radius_m  double precision not null,
    alert_on  text not null default 'both' check (alert_on in ('enter','exit','both'))
);

create table if not exists device_health (
    id           bigserial primary key,
    device_id    text not null references devices(id) on delete cascade,
    reported_at  timestamptz not null,
    battery_pct  int,
    signal_csq   int,
    queue_depth  int,
    uptime_s     int,
    reset_reason text
);

-- ============================================================= access ======
-- Explicit grants. No implicit "sees everything" — every RLS policy below
-- routes through this table.
create table if not exists device_access (
    user_id   uuid not null references auth.users(id) on delete cascade,
    device_id text not null references devices(id) on delete cascade,
    role      text not null default 'parent' check (role in ('parent','viewer')),
    escalation_order int not null default 1,        -- 1 = primary, 2 = secondary
    phone     text,                                 -- where SMS and voice rungs go
    primary key (user_id, device_id)
);

create table if not exists access_log (
    id        bigserial primary key,
    user_id   uuid references auth.users(id) on delete set null,
    device_id text,
    action    text not null,
    at        timestamptz not null default now()
);

create table if not exists integration_health (
    integration     text primary key,
    last_success_at timestamptz,
    last_error      text,
    status          text not null default 'unknown'
);

-- =============================================================== helper ====
create or replace function has_device_access(d text)
returns boolean
language sql
security definer
set search_path = public
stable
as $$
    select exists (
        select 1 from device_access
        where user_id = auth.uid() and device_id = d
    );
$$;
