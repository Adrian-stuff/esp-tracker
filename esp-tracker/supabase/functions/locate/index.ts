// "Locate now" — the SMS downlink, triggered from the dashboard.
//
// Verifies the caller's JWT and their device grant before spending an SMS.
// This is the one place a parent action reaches the device, and the device's
// number is a remote-control interface for anyone who learns it — hence
// SMS_CMD_SECRET in the payload and a rate limit here, so repeated taps cannot
// flatten the battery.

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
    .select("msisdn").eq("id", device_id).maybeSingle();
  if (!dev?.msisdn) return json({ error: "no number registered for this device" }, 400);

  const r = await sendLocateCommand(dev.msisdn);
  await admin.from("access_log").insert({ user_id: user.id, device_id, action: "locate" });
  if (!r.ok) return json({ error: `could not reach the device: ${r.error}` }, 502);

  return json({ ok: true, eta_s: 20 });
});

function json(b: unknown, status = 200) {
  return new Response(JSON.stringify(b), {
    status, headers: { "Content-Type": "application/json" },
  });
}
