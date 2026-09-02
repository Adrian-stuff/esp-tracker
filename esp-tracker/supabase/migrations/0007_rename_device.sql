-- Lets a signed-in staff member with access to a gate scanner rename it from
-- the attendance dashboard, without granting a raw client UPDATE policy on
-- devices (which would also expose token_hash, msisdn and other fields no
-- browser should be able to touch).
--
-- This does NOT reach the scanner itself — WiFi credentials, the API base URL
-- and the SIM800 numbers only ever change through the device's own captive
-- portal (see scanner/src/net.cpp). There is no remote command channel to the
-- firmware today; this function only renames the row the dashboard shows.
create or replace function rename_device(d text, new_name text)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    if not has_device_access(d) then
        raise exception 'no access to this device';
    end if;
    if new_name is null or length(trim(new_name)) = 0 then
        raise exception 'name cannot be empty';
    end if;
    update devices set name = trim(new_name) where id = d;
end;
$$;

grant execute on function rename_device(text, text) to authenticated;
