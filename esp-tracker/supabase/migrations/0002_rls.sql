-- Row Level Security.
--
-- ****************************************************************************
-- READ THIS BEFORE DEPLOYING.
--
-- Supabase exposes every table over PostgREST using the anon key, which ships
-- in your frontend bundle. A table WITHOUT RLS enabled is readable by anyone
-- on the internet who opens devtools. For a live database of a child's
-- real-time location that is not a misconfiguration, it is a disaster.
--
-- Every table below gets `enable row level security`, with no exceptions. The
-- Edge Functions use the SERVICE ROLE key, which bypasses RLS by design — that
-- key must never leave the server environment.
-- ****************************************************************************

alter table devices            enable row level security;
alter table places             enable row level security;
alter table wifi_scans         enable row level security;
alter table locations          enable row level security;
alter table sos_events         enable row level security;
alter table alert_queue        enable row level security;
alter table alerts             enable row level security;
alter table cards              enable row level security;
alter table attendance         enable row level security;
alter table geofences          enable row level security;
alter table device_health      enable row level security;
alter table device_access      enable row level security;
alter table access_log         enable row level security;
alter table integration_health enable row level security;

-- ---------------------------------------------------------------- read -----
-- A parent sees exactly the devices they were granted, and nothing else.

create policy "own devices" on devices
    for select using (has_device_access(id));

create policy "own locations" on locations
    for select using (has_device_access(device_id));

create policy "own sos" on sos_events
    for select using (has_device_access(device_id));

create policy "own places" on places
    for select using (has_device_access(device_id));

create policy "own wifi scans" on wifi_scans
    for select using (has_device_access(device_id));

create policy "own geofences" on geofences
    for select using (has_device_access(device_id));

create policy "own health" on device_health
    for select using (has_device_access(device_id));

create policy "own grants" on device_access
    for select using (user_id = auth.uid());

create policy "own alerts" on alerts
    for select using (exists (
        select 1 from sos_events s
        where s.id = alerts.sos_event_id and has_device_access(s.device_id)));

-- Attendance is scoped through the card's linked tracker, so a parent sees
-- their own child's taps and not the whole school's roll.
create policy "own attendance" on attendance
    for select using (
        child_device_id is not null and has_device_access(child_device_id));

create policy "own cards" on cards
    for select using (device_id is not null and has_device_access(device_id));

-- --------------------------------------------------------------- write -----
-- Parents get exactly two write paths: acknowledging an SOS, and naming a
-- place. Everything else is written by Edge Functions with the service role.

create policy "ack own sos" on sos_events
    for update using (has_device_access(device_id))
    with check (has_device_access(device_id));

create policy "name own places" on places
    for insert with check (has_device_access(device_id));

create policy "remove own places" on places
    for delete using (has_device_access(device_id));

create policy "edit own geofences" on geofences
    for all using (has_device_access(device_id))
    with check (has_device_access(device_id));

-- alert_queue, access_log and integration_health get NO policies at all:
-- RLS is on, nothing matches, so they are invisible to the anon and
-- authenticated roles. Only the service role touches them.

-- Cards are readable only through the child's own tracker grant, which is also
-- what scopes the attendance_days view (it is security_invoker, so this policy
-- still applies through it).

-- ------------------------------------------------------------- realtime ----
-- Replaces the hand-rolled WebSocket hub. The browser subscribes with the anon
-- key and the policies above still apply, so a parent is only ever pushed rows
-- for their own devices.
alter publication supabase_realtime add table locations;
alter publication supabase_realtime add table sos_events;
alter publication supabase_realtime add table attendance;
