-- Attendance direction, inferred server-side.
--
-- The scanner does NOT send in/out, and should not. Three reasons, and the
-- third is the one that actually forces it:
--
--   1. The device would have to hold per-card state, which it loses on reboot.
--   2. One reader cannot see which side of the gate a child walked from.
--   3. Taps buffered during a Wi-Fi outage arrive LATE and OUT OF ORDER. A tap
--      from 07:58 can land after one from 15:10. Anything that decides
--      direction at arrival time gets those backwards.
--
-- So direction is recomputed from the card's own history for that day, ordered
-- by recorded_at: odd taps are 'in', even are 'out'. Re-running it is
-- idempotent, which is what makes late arrivals self-correcting.

create or replace function recompute_attendance_direction(p_card text, p_day date)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    with ordered as (
        select id,
               row_number() over (order by recorded_at, id) as rn
        from attendance
        where card_uid = p_card
          and (recorded_at at time zone 'Asia/Manila')::date = p_day
    )
    update attendance a
       set direction = case when o.rn % 2 = 1 then 'in' else 'out' end
      from ordered o
     where a.id = o.id
       and a.direction is distinct from (case when o.rn % 2 = 1 then 'in' else 'out' end);
end;
$$;

-- AFTER INSERT only, so the UPDATE inside cannot re-trigger it.
create or replace function attendance_after_insert()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
    perform recompute_attendance_direction(
        new.card_uid,
        (new.recorded_at at time zone 'Asia/Manila')::date);
    return null;
end;
$$;

drop trigger if exists trg_attendance_direction on attendance;
create trigger trg_attendance_direction
    after insert on attendance
    for each row execute function attendance_after_insert();

create index if not exists ix_att_card_day
    on attendance (card_uid, recorded_at);

-- ---------------------------------------------------------------------------
-- One row per child per day: when they arrived, when they left, how many taps.
-- This is what a parent actually wants to see — not a list of raw scans.
-- ---------------------------------------------------------------------------
create or replace view attendance_days
with (security_invoker = true) as
select
    a.card_uid,
    c.child_name,
    a.child_device_id,
    (a.recorded_at at time zone 'Asia/Manila')::date        as day,
    min(a.recorded_at) filter (where a.direction = 'in')    as first_in,
    max(a.recorded_at) filter (where a.direction = 'out')   as last_out,
    count(*)                                                as taps
from attendance a
left join cards c on c.card_uid = a.card_uid
group by a.card_uid, c.child_name, a.child_device_id,
         (a.recorded_at at time zone 'Asia/Manila')::date;

-- security_invoker means the view runs with the CALLER's permissions, so the
-- RLS policy on `attendance` still applies. Without it a view would be a hole
-- straight through row level security — a classic way to leak a whole school.
