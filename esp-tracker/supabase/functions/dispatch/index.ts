// The escalation ladder, as a queue drainer.
//
// Called by pg_cron every minute (see migrations/0003_cron.sql). This is what
// replaced the in-process asyncio ladder: a serverless function is killed the
// moment it returns, so nothing can sleep for 300 s.
//
// The mechanism that matters is still the CUT, not the ladder. A rung only
// fires if its SOS is still 'open'; acknowledging sets status and every
// remaining rung is skipped on its next visit. Fanning out all channels at once
// and hoping is not a design.
//
// TIMING: cron granularity is 60 s, so a rung due at t+60 fires in t+60..t+120.
// That is fine for rungs 2 and up — rung 1 already fired synchronously inside
// the ingest request. If you need tighter, replace the cron with delayed
// callbacks (QStash and similar schedule to the second).

import { createClient } from "jsr:@supabase/supabase-js@2";
import { sendSms, sosText } from "../_shared/sms.ts";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

const MAX_ATTEMPTS = 3;

Deno.serve(async () => {
  const now = new Date().toISOString();

  const { data: due } = await db.from("alert_queue")
    .select("id,sos_event_id,channel,attempts")
    .eq("status", "pending").lte("due_at", now)
    .order("due_at", { ascending: true }).limit(50);

  let sent = 0, skipped = 0;

  for (const row of due ?? []) {
    const { data: sos } = await db.from("sos_events")
      .select("id,event_id,device_id,status,devices(name,child_name),best_location_id")
      .eq("id", row.sos_event_id).maybeSingle();

    // THE CUT. Anything not open — acknowledged, resolved, marked a false
    // alarm — stops the ladder here.
    if (!sos || sos.status !== "open") {
      await db.from("alert_queue").update({ status: "cancelled" }).eq("id", row.id);
      skipped++;
      continue;
    }

    await db.from("alert_queue")
      .update({ claimed_at: now, attempts: row.attempts + 1 }).eq("id", row.id);

    let loc = null;
    if (sos.best_location_id) {
      const { data } = await db.from("locations")
        .select("lat,lon,accuracy_m,source").eq("id", sos.best_location_id).maybeSingle();
      loc = data;
    }
    const dev: any = sos.devices;
    const text = sosText(dev?.child_name ?? dev?.name ?? sos.device_id, loc);

    const order = (row.channel === "sms2" || row.channel === "voice2") ? 2 : 1;
    const { data: contacts } = await db.from("device_access")
      .select("phone").eq("device_id", sos.device_id).eq("escalation_order", order);

    const targets = (contacts ?? []).filter((c) => c.phone);
    if (!targets.length) {
      await db.from("alert_queue").update({
        status: "skipped", error: `no escalation_order ${order} contact`,
      }).eq("id", row.id);
      skipped++;
      continue;
    }

    let ok = true;
    for (const c of targets) {
      if (row.channel === "voice" || row.channel === "voice2") {
        // TODO: Twilio voice + TTS. Costs real money — it is deliberately the
        // last rung, and only reached when nobody has acknowledged in 3 minutes.
        await db.from("alerts").insert({
          sos_event_id: sos.id, channel: "voice", recipient: c.phone!,
          delivery_status: "not_implemented",
        });
        continue;
      }
      const r = await sendSms(c.phone!, text);
      ok = ok && r.ok;
      await db.from("alerts").insert({
        sos_event_id: sos.id, channel: "sms", recipient: c.phone!,
        provider_msg_id: r.msgId, delivery_status: r.ok ? "sent" : "failed", error: r.error,
      });
    }

    if (ok) { await db.from("alert_queue").update({ status: "sent" }).eq("id", row.id); sent++; }
    else if (row.attempts + 1 >= MAX_ATTEMPTS) {
      await db.from("alert_queue").update({ status: "failed" }).eq("id", row.id);
    }
    // Otherwise it stays pending and the next minute retries it.
  }

  await db.from("integration_health").upsert({
    integration: "dispatch", last_success_at: now, status: "ok",
  });

  return new Response(JSON.stringify({ due: due?.length ?? 0, sent, skipped }), {
    headers: { "Content-Type": "application/json" },
  });
});
