// Device-relayed SMS — the claim/ack half. Ported from server/app/relay.py's
// claim() and ack(), which is the source of truth for this exact logic.
//
// Devices authenticate with their own bearer token (sha256 in devices.token_hash),
// not a Supabase Auth JWT — this function MUST be deployed with --no-verify-jwt,
// same reasoning as ingest and roster. Without it every device call gets
// rejected at the platform gateway before this code ever runs (that was a real
// bug: ingest and roster shipped with the default verify_jwt=true and were
// unreachable by real firmware until redeployed with the flag).
//
// Two routes, one function, because Supabase Edge Functions are one entrypoint
// per directory:
//   GET  /functions/v1/outbox?limit=N        -> claim()
//   POST /functions/v1/outbox/{id}/ack       -> ack()

import { createClient } from "jsr:@supabase/supabase-js@2";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

const RELAY_LEASE_S = 60;       // claim lifetime — matches server/app/config.py
const RELAY_MAX_ATTEMPTS = 2;   // matches server/app/config.py

const enc = new TextEncoder();
async function sha256(s: string) {
  const buf = await crypto.subtle.digest("SHA-256", enc.encode(s));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

Deno.serve(async (req) => {
  const authz = req.headers.get("authorization") ?? "";
  if (!authz.startsWith("Bearer ")) return json({ error: "missing device token" }, 401);
  const { data: device } = await db.from("devices").select("*")
    .eq("token_hash", await sha256(authz.slice(7))).eq("active", true).maybeSingle();
  if (!device) return json({ error: "unknown device" }, 401);

  const url = new URL(req.url);
  const parts = url.pathname.split("/").filter(Boolean);
  const i = parts.indexOf("outbox");
  const msgId = i >= 0 ? parts[i + 1] : undefined;
  const isAck = i >= 0 && parts[i + 2] === "ack";

  if (req.method === "POST" && isAck && msgId) return ack(device, msgId, req);
  if (req.method === "GET" && !msgId) return claim(device, url);
  return json({ error: "not found" }, 404);
});

async function claim(device: { id: string; kind: string }, url: URL) {
  const limit = Math.min(10, Number(url.searchParams.get("limit") ?? 3) || 3);
  const now = new Date().toISOString();

  let q = db.from("outbox").select("id,to_number,body")
    .in("status", ["pending", "claimed"])
    .lt("attempts", RELAY_MAX_ATTEMPTS)
    .or(`lease_until.is.null,lease_until.lt.${now}`)
    .gt("fallback_after", now)
    .order("created_at", { ascending: true })
    .limit(limit);
  // A tracker may only relay messages about its own child — see the table
  // comment in 0005_outbox.sql. A scanner is fixed shared infrastructure and
  // may relay anything, so it gets no filter here.
  if (device.kind !== "scanner") q = q.eq("child_device_id", device.id);

  const { data: rows } = await q;
  const out = [];
  for (const r of rows ?? []) {
    const leaseUntil = new Date(Date.now() + RELAY_LEASE_S * 1000).toISOString();
    const { error } = await db.from("outbox").update({
      status: "claimed", claimed_by: device.id, lease_until: leaseUntil,
    }).eq("id", r.id).in("status", ["pending", "claimed"]);
    if (!error) out.push({ id: r.id, to: r.to_number, body: r.body });
  }
  return json({ messages: out });
}

async function ack(device: { id: string }, msgId: string, req: Request) {
  const body = await req.json().catch(() => ({}));
  const { data: row } = await db.from("outbox").select("claimed_by,attempts")
    .eq("id", msgId).maybeSingle();
  if (!row || row.claimed_by !== device.id) return json({ error: "not your claim" }, 404);

  if (body.sent) {
    await db.from("outbox").update({ status: "sent", sent_at: new Date().toISOString() }).eq("id", msgId);
  } else {
    // Released, not burned: another device or the sweep should still try —
    // see server/app/relay.py's ack() for why this matters.
    await db.from("outbox").update({
      status: "pending", lease_until: null,
      attempts: row.attempts + 1, error: body.error ?? null,
    }).eq("id", msgId);
  }
  return json({ ok: true });
}

function json(b: unknown, status = 200) {
  return new Response(JSON.stringify(b), {
    status, headers: { "Content-Type": "application/json" },
  });
}
