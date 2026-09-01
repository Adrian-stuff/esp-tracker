// Device pairing — the only way a device_access row gets created outside of
// an admin doing it by hand.
//
// The caller is a signed-in parent (their own JWT, not a device token — this
// is the one Edge Function browsers call directly with a user session rather
// than a device bearer). The claim CODE is the proof of possession: whoever
// provisioned the device handed it to this parent through some other channel
// already, so typing it in here is what "I actually have this device" means.
//
// Runs with the SERVICE ROLE key because device_access has no client INSERT
// policy — an authenticated user granting themselves access to an arbitrary
// device_id, no proof required, would defeat the entire point of RLS here.

import { createClient } from "jsr:@supabase/supabase-js@2";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);
// A second client on the ANON key, used only to resolve the caller's own JWT
// to a user — never to touch a table. Keeps the service-role client's use
// limited to the one write this function exists to make.
const anon = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_ANON_KEY")!,
);

Deno.serve(async (req) => {
  const authz = req.headers.get("authorization") ?? "";
  const jwt = authz.replace(/^Bearer\s+/i, "");
  const { data: { user }, error: authErr } = await anon.auth.getUser(jwt);
  if (authErr || !user) return json({ error: "sign in first" }, 401);

  const body = await req.json().catch(() => null);
  const code = (body?.code ?? "").trim();
  const phone = (body?.phone ?? "").trim() || null;
  if (!code) return json({ error: "enter the code from the device" }, 400);

  const { data: device } = await db.from("devices")
    .select("id,name,child_name,kind")
    .eq("claim_code", code).eq("active", true).maybeSingle();
  if (!device) return json({ error: "that code doesn't match a device" }, 404);

  // Later claimants land as secondary/tertiary contacts rather than all
  // colliding on escalation_order 1 — e.g. a second parent pairing after the
  // first still gets a real place in the SOS ladder instead of tying for it.
  const { count } = await db.from("device_access")
    .select("*", { count: "exact", head: true }).eq("device_id", device.id);

  const { error } = await db.from("device_access").upsert({
    user_id: user.id, device_id: device.id, phone,
    escalation_order: (count ?? 0) === 0 ? 1 : (count as number) + 1,
  }, { onConflict: "user_id,device_id" });
  if (error) return json({ error: error.message }, 500);

  return json({ ok: true, device: { id: device.id, name: device.name, child_name: device.child_name } });
});

function json(b: unknown, status = 200) {
  return new Response(JSON.stringify(b), {
    status, headers: { "Content-Type": "application/json" },
  });
}
