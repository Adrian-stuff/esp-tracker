-- Presentation convenience: every new signup automatically gets dashboard
-- visibility into the two real devices used for live demos (the actual
-- tracker + actual scanner), without the claim-code proof-of-possession
-- step in supabase/functions/claim/index.ts. That flow stays the correct
-- production model for a genuine parent pairing their own hardware; this
-- trigger exists ONLY so a presentation audience can sign up on the spot
-- and immediately see the dashboard live, with zero extra steps.
--
-- SAFETY: phone is always NULL here, never populated from anything.
-- Real SOS escalation (see supabase/functions/dispatch/index.ts) only ever
-- texts/calls device_access rows whose phone is non-null — dispatch's own
-- `.filter((c) => c.phone)` drops every null one before sending anything.
-- So a demo signup can VIEW live location/SOS data but can never be
-- silently pulled into a real family's emergency SMS/voice ladder, no
-- matter how many accounts get created during a presentation. role is
-- 'viewer', not 'parent' — nothing in this project currently reads or
-- enforces that field, but there's no reason to grant the stronger label
-- to an account nobody vetted.
--
-- If this project is ever used for real families alongside live demos,
-- DROP TRIGGER on_auth_user_created_link_devices ON auth.users; first —
-- this makes every signup, including a real parent's, see the demo tracker
-- and scanner by default, which is fine for a presentation and wrong for
-- production.

create or replace function link_new_user_to_demo_devices()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
  insert into device_access (user_id, device_id, role, phone, escalation_order)
  select
    new.id, d.id, 'viewer', null,
    coalesce((select max(escalation_order) from device_access where device_id = d.id), 0) + 1
  from devices d
  where d.id in ('tracker-01', 'scanner-gate-01')
  on conflict (user_id, device_id) do nothing;
  return new;
end;
$$;

drop trigger if exists on_auth_user_created_link_devices on auth.users;
create trigger on_auth_user_created_link_devices
  after insert on auth.users
  for each row execute function link_new_user_to_demo_devices();
