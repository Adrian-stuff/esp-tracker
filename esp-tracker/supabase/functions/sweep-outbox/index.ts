// Pays for anything a device did not deliver in time. Ported from
// server/app/relay.py's sweep(), which runs as an asyncio loop there; here
// it's what 0005_outbox.sql's pg_cron job calls once a minute instead.
//
// Deliberately left at the default verify_jwt=true. Unlike ingest/roster/
// outbox — which devices call with their own bearer token, not a Supabase
// JWT, and so need --no-verify-jwt — this function's only legitimate caller
// is pg_cron, which authenticates with the real service_role JWT (see the
// migration). Requiring a valid JWT here is a free "only cron may trigger a
// real SMS spend" gate; disabling it would let anyone on the internet hit
// this endpoint and force paid sends for whatever is currently queued.

import { createClient } from "jsr:@supabase/supabase-js@2";
import { sendSms } from "../_shared/sms.ts";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

Deno.serve(async () => {
  const now = new Date().toISOString();
  const { data: due } = await db.from("outbox").select("*")
    .in("status", ["pending", "claimed"])
    .lte("fallback_after", now)
    .order("created_at", { ascending: true })
    .limit(20);

  let sent = 0;
  for (const m of due ?? []) {
    const r = await sendSms(m.to_number, m.body);
    await db.from("outbox").update({
      status: r.ok ? "provider_sent" : "failed",
      sent_at: r.ok ? new Date().toISOString() : null,
      error: r.ok ? null : r.error,
    }).eq("id", m.id);
    if (r.ok) sent++;
  }
  return new Response(JSON.stringify({ swept: (due ?? []).length, sent }), {
    headers: { "Content-Type": "application/json" },
  });
});
