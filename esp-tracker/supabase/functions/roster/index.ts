// Roster — hashed UIDs of enrolled cards, for the gate's local check.
//
// Sends salted HASHES. Neither the names nor the UIDs themselves leave the
// server, so the scanner can answer "is this card enrolled?" immediately while
// a stolen device reveals a set of hashes rather than a list of children.
//
// ROSTER_SALT must match scanner/include/config.h. It is not a secret from
// whoever holds the device — it only stops a precomputed table of every 4-byte
// MIFARE UID.
//
// Honest limit: card UIDs are short and low-entropy, so these are
// brute-forceable by anyone holding the device. What they do NOT carry is
// identity — no names, no classes, no parents. That is the whole claim.

import { createClient } from "jsr:@supabase/supabase-js@2";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

const enc = new TextEncoder();
async function sha256hex(s: string) {
  const b = await crypto.subtle.digest("SHA-256", enc.encode(s));
  return [...new Uint8Array(b)].map((x) => x.toString(16).padStart(2, "0")).join("");
}

Deno.serve(async (req) => {
  const auth = req.headers.get("authorization") ?? "";
  if (!auth.startsWith("Bearer ")) return json({ error: "missing device token" }, 401);

  const { data: device } = await db.from("devices").select("id,kind")
    .eq("token_hash", await sha256hex(auth.slice(7))).eq("active", true).maybeSingle();
  if (!device) return json({ error: "unknown device" }, 401);
  if (device.kind !== "scanner") return json({ error: "not a scanner" }, 403);

  const salt = Deno.env.get("ROSTER_SALT") ?? "change-me-too";
  const { data: cards } = await db.from("cards").select("card_uid").eq("active", true);
  const h = await Promise.all(
    (cards ?? []).map(async (c) => (await sha256hex(salt + c.card_uid)).slice(0, 8)));
  return json({ h, count: h.length });
});

function json(b: unknown, status = 200) {
  return new Response(JSON.stringify(b), {
    status, headers: { "Content-Type": "application/json" },
  });
}
