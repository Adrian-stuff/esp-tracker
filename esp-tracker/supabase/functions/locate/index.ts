// "Locate now" — the SMS downlink, triggered from the dashboard.
//
// Verifies the caller's JWT and their device grant before spending an SMS.
// This is the one place a parent action reaches the device, and the device's
// number is a remote-control interface for anyone who learns it — hence
// SMS_CMD_SECRET in the payload and a rate limit here, so repeated taps cannot
// flatten the battery.
//
// MOCK FALLBACK: a device with no devices.msisdn (every device dev-mock's
// ensureDevice() creates — see functions/dev-mock/index.ts — never sets one)
// can never actually receive the real SMS command, so used to just fail with
// "no number registered". Presentation devices need "Locate now" to visibly
// do something too, so that case now inserts a location row directly instead
// — jittered a little from wherever the device last was, source:'manual' (an
// already-existing, already-displayed source value — see tracker.js's SOURCE
// map — chosen because it is honestly what this is: not a real GPS/Wi-Fi/cell
// fix, a synthesized stand-in for one). A REAL device with a real msisdn
// always takes the real SMS path; this never silently replaces that.

import { createClient } from "jsr:@supabase/supabase-js@2";
import { sendLocateCommand } from "../_shared/sms.ts";

const admin = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

const RATE_LIMIT_S = 60;

Deno.serve(async (req) => {
  const jwt = (req.headers.get("authorization") ?? "").replace("Bearer ", "");
  const { data: { user } } = await admin.auth.getUser(jwt);
  if (!user) return json({ error: "not signed in" }, 401);

  const { device_id } = await req.json().catch(() => ({}));
  if (!device_id) return json({ error: "device_id required" }, 400);

  const { data: grant } = await admin.from("device_access")
    .select("device_id").eq("user_id", user.id).eq("device_id", device_id).maybeSingle();
  if (!grant) return json({ error: "no access to this device" }, 403);

  const since = new Date(Date.now() - RATE_LIMIT_S * 1000).toISOString();
  const { count } = await admin.from("access_log")
    .select("id", { count: "exact", head: true })
    .eq("device_id", device_id).eq("action", "locate").gte("at", since);
  if ((count ?? 0) > 0) return json({ error: "already asked in the last minute" }, 429);

  const { data: dev } = await admin.from("devices")
    .select("msisdn, battery_pct").eq("id", device_id).maybeSingle();
  if (!dev) return json({ error: "unknown device" }, 404);

  await admin.from("access_log").insert({ user_id: user.id, device_id, action: "locate" });

  // ---------------------------------------------------- mock fallback ----
  if (!dev.msisdn) {
    const { data: last } = await admin.from("locations")
      .select("lat,lon,accuracy_m").eq("device_id", device_id)
      .order("recorded_at", { ascending: false }).limit(1).maybeSingle();
    // ~0-15m jitter (0.00005deg =~ 5.5m at the equator) around the last known
    // point, or Manila as a reasonable default for a device with no history
    // at all yet.
    const baseLat = last?.lat ?? 14.5995, baseLon = last?.lon ?? 120.9842;
    const lat = baseLat + (Math.random() - 0.5) * 0.0001;
    const lon = baseLon + (Math.random() - 0.5) * 0.0001;
    const now = new Date().toISOString();
    const eventId = `${device_id}-locate-mock-${Date.now()}`;
    const battery = dev.battery_pct != null ? Math.max(1, dev.battery_pct - 1) : 85;

    const { error } = await admin.from("locations").insert({
      event_id: eventId, device_id, lat, lon,
      accuracy_m: last?.accuracy_m ?? 15, source: "manual",
      recorded_at: now, received_at: now, battery_pct: battery,
    });
    if (error) return json({ error: error.message }, 500);
    await admin.from("devices").update({ last_seen_at: now, battery_pct: battery }).eq("id", device_id);

    return json({ ok: true, mock: true, eta_s: 1 });
  }

  // ------------------------------------------------------- real device ----
  const r = await sendLocateCommand(dev.msisdn);
  if (!r.ok) return json({ error: `could not reach the device: ${r.error}` }, 502);

  return json({ ok: true, mock: false, eta_s: 20 });
});

function json(b: unknown, status = 200) {
  return new Response(JSON.stringify(b), {
    status, headers: { "Content-Type": "application/json" },
  });
}
