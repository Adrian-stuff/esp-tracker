// Mock location pull — tracker on WiFi fetches the latest mock location
// inserted by dev-mock. Used during demos/presentations when GPS is indoors
// and can't get a real fix.
//
// Auth: DEVICE_TOKEN bearer (sha256 match against devices.token_hash),
// same as ingest. This is the device itself asking "do you have a mock
// location for me?" — not a user, not dev-mock's secret.
//
// Returns the most recent location row for this device, or 404 if none
// exists yet. The tracker caches it locally and uses it in SMS reports
// when GPS fails.

import { createClient } from "jsr:@supabase/supabase-js@2";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

const enc = new TextEncoder();
async function sha256(s: string) {
  const buf = await crypto.subtle.digest("SHA-256", enc.encode(s));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

Deno.serve(async (req) => {
  if (req.method !== "GET") {
    return new Response(JSON.stringify({ error: "GET only" }), {
      status: 405, headers: { "Content-Type": "application/json" },
    });
  }

  const auth = (req.headers.get("authorization") ?? "").replace("Bearer ", "");
  if (!auth) {
    return new Response(JSON.stringify({ error: "missing bearer token" }), {
      status: 401, headers: { "Content-Type": "application/json" },
    });
  }

  const tokenHash = await sha256(auth);
  const { data: dev } = await db.from("devices")
    .select("id").eq("token_hash", tokenHash).maybeSingle();
  if (!dev) {
    return new Response(JSON.stringify({ error: "unknown device" }), {
      status: 401, headers: { "Content-Type": "application/json" },
    });
  }

  // Latest location for this device — whatever source (gnss, wifi, manual,
  // mock). The tracker just needs coordinates when its own GPS can't fix.
  const { data: loc } = await db.from("locations")
    .select("lat,lon,accuracy_m,source,recorded_at")
    .eq("device_id", dev.id)
    .order("recorded_at", { ascending: false })
    .limit(1)
    .maybeSingle();

  if (!loc) {
    return new Response(JSON.stringify({ error: "no location yet" }), {
      status: 404, headers: { "Content-Type": "application/json" },
    });
  }

  return new Response(JSON.stringify({ ok: true, ...loc }), {
    status: 200, headers: { "Content-Type": "application/json" },
  });
});
