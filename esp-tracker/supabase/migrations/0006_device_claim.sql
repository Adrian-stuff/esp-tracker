-- Self-service device pairing.
--
-- Until now, device_access rows only ever came from an admin inserting them
-- directly (see server/seed.py). That's fine for a demo, not for a parent
-- creating their own account. A claim code is the proof-of-possession step:
-- whoever provisions a device hands the code to the parent (printed, texted,
-- whatever channel already exists), and the parent spends it once, in the
-- dashboard, to link their new account to that specific device. Nothing about
-- RLS changes — an account with no device_access row still sees nothing.

alter table devices add column if not exists claim_code text;

-- Unique only where set, so multiple devices can each have NULL without
-- colliding.
create unique index if not exists ix_devices_claim_code
    on devices (claim_code) where claim_code is not null;
