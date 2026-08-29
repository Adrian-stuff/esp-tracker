// Device ingest — the only endpoint the tracker and scanner talk to.
//
// Runs with the SERVICE ROLE key, so it bypasses RLS. That key must never leave
// this environment: it is set with `supabase secrets set`, not in the client.
//
// Devices are not Supabase Auth users. They authenticate with a per-device
// bearer token whose sha256 is stored in devices.token_hash — no shared fleet
// secret, so one recovered device cannot compromise the others.
//
// CONTRACT: the 200 response IS the ack. The device releases an event from its
// flash queue on 200 and on nothing else.

import { createClient } from "jsr:@supabase/supabase-js@2";
import { sendSms, sosText } from "../_shared/sms.ts";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

const enc = new TextEncoder();
async function sha256(s: string) {
  const buf = await crypto.subtle.digest("SHA-256", enc.encode(s));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
async function bssidHash(b: string) {
  return (await sha256(b.toLowerCase())).slice(0, 16);
}
const ts = (epoch: number) => new Date(epoch * 1000).toISOString();

// A place matches when enough of its registered BSSIDs are visible. When it
// hits, no geolocation API is called at all — cheapest, fastest and most
// private outcome in one.
async function matchPlace(deviceId: string, hashes: string[]) {
  const { data: places } = await db.from("places")
    .select("id,name,lat,lon,radius_m,bssid_set,ble_anchor_id")
    .eq("device_id", deviceId).not("bssid_set", "is", null);
  const seen = new Set(hashes);
  for (const p of places ?? []) {
    const reg: string[] = p.bssid_set ?? [];
    if (!reg.length) continue;
    const inter = reg.filter((h) => seen.has(h)).length;
    if (inter >= Math.max(2, Math.floor(reg.length / 3))) {
      return { place: p, fix: { lat: p.lat, lon: p.lon, accuracy_m: p.radius_m ?? 40,
                                source: p.ble_anchor_id ? "ble_anchor" : "wifi" } };
    }
  }
  return null;
}

Deno.serve(async (req) => {
  const auth = req.headers.get("authorization") ?? "";
  if (!auth.startsWith("Bearer ")) return json({ error: "missing device token" }, 401);

  const { data: device } = await db.from("devices").select("*")
    .eq("token_hash", await sha256(auth.slice(7))).eq("active", true).maybeSingle();
  if (!device) return json({ error: "unknown device" }, 401);

  const body = await req.json().catch(() => null);
  if (!body) return json({ error: "bad json" }, 400);

  // ------------------------------------------------------------ taps -------
  if (Array.isArray(body.taps)) {
    if (device.kind !== "scanner") return json({ error: "not a scanner" }, 403);
    const accepted: string[] = [];
    for (const t of body.taps) {
      const { data: card } = await db.from("cards").select("device_id,child_name")
        .eq("card_uid", t.card_uid).eq("active", true).maybeSingle();
      // upsert on the unique event_id: a retried tap must not become a second
      // attendance record.
      // Direction is NOT taken from the device. A trigger recomputes it from
      // the card's own history for that day, which is the only thing that
      // survives the scanner rebooting and handles taps that were buffered
      // offline and arrived out of order. See migrations/0004_attendance.sql.
      await db.from("attendance").upsert({
        event_id: t.id, scanner_id: device.id, card_uid: t.card_uid,
        child_device_id: card?.device_id ?? null,
        recorded_at: ts(t.recorded_at),
      }, { onConflict: "event_id", ignoreDuplicates: true });
      accepted.push(t.id);
    }
    await db.from("devices").update({ last_seen_at: new Date().toISOString() }).eq("id", device.id);
    return json({ accepted });
  }

  // ----------------------------------------------------------- events ------
  const events = body.events ?? [];
  const accepted: string[] = [];

  for (const ev of events) {
    let { lat, lon, accuracy_m, source } = ev;
    let placeId: number | null = null;

    if (Array.isArray(ev.wifi) && ev.wifi.length) {
      const hashes = await Promise.all(ev.wifi.map((a: any) => bssidHash(a.bssid)));
      const m = await matchPlace(device.id, hashes);
      if (m) {
        placeId = m.place.id;
        if (lat == null) ({ lat, lon, accuracy_m, source } = m.fix);
      }
      // Keep the scan so the parent can name it later. BSSIDs hashed; the SSID
      // stays readable because it is the only part a parent recognises.
      await db.from("wifi_scans").insert({
        device_id: device.id, recorded_at: ts(ev.recorded_at), place_id: placeId,
        aps: await Promise.all(ev.wifi.map(async (a: any) => ({
          h: await bssidHash(a.bssid), ssid: (a.ssid ?? "").slice(0, 32), rssi: a.rssi,
        }))),
      });
      // TODO: unresolved scans -> geolocation provider, key stays server-side.
    }

    let locId: number | null = null;
    if (lat != null && lon != null) {
      const { data: loc } = await db.from("locations").upsert({
        event_id: ev.id, device_id: device.id, lat, lon,
        accuracy_m: accuracy_m ?? 100, source: source ?? "cell", place_id: placeId,
        recorded_at: ts(ev.recorded_at), battery_pct: ev.battery_pct, speed_mps: ev.speed_mps,
      }, { onConflict: "event_id", ignoreDuplicates: true }).select("id").maybeSingle();
      locId = loc?.id ?? null;
    }

    if (ev.kind === "sos") await handleSos(device, ev, locId, { lat, lon, accuracy_m, source });

    if (ev.kind === "health") {
      await db.from("device_health").insert({
        device_id: device.id, reported_at: ts(ev.recorded_at), battery_pct: ev.battery_pct,
        signal_csq: ev.signal_csq, queue_depth: ev.queue_depth,
      });
    }
    accepted.push(ev.id);
  }

  const last = events[events.length - 1];
  await db.from("devices").update({
    last_seen_at: new Date().toISOString(),
    ...(last?.battery_pct != null   ? { battery_pct: last.battery_pct } : {}),
    ...(last?.signal_csq != null    ? { signal_csq: last.signal_csq } : {}),
    ...(last?.balance_pesos != null ? { balance_pesos: last.balance_pesos } : {}),
  }).eq("id", device.id);

  return json({ accepted });
});

async function handleSos(device: any, ev: any, locId: number | null, fix: any) {
  const { data: sos, error } = await db.from("sos_events").insert({
    event_id: ev.id, device_id: device.id, triggered_at: ts(ev.recorded_at),
    latency_ms: Math.max(0, Date.now() - ev.recorded_at * 1000),
    first_location_id: locId, best_location_id: locId,
    device_sms_sent: !!ev.device_sms_sent, is_drill: !!ev.is_drill,
  }).select("id").maybeSingle();

  if (error || !sos) return;   // duplicate event_id — never start a second ladder

  const loc = fix?.lat != null ? fix : null;
  const text = sosText(device.child_name ?? device.name, loc);

  const { data: contacts } = await db.from("device_access")
    .select("phone,escalation_order").eq("device_id", device.id);

  // RUNG 1 fires HERE, synchronously, inside this request. It is never queued:
  // cron granularity is 60 s and the first alert must not wait for a scheduler.
  // The realtime push is implicit — the insert above already reached every
  // subscribed dashboard.
  await db.from("alerts").insert({
    sos_event_id: sos.id, channel: "realtime", recipient: "dashboard",
  });

  if (!ev.device_sms_sent) {
    for (const c of (contacts ?? []).filter((c) => c.escalation_order === 1 && c.phone)) {
      const r = await sendSms(c.phone!, text);
      await db.from("alerts").insert({
        sos_event_id: sos.id, channel: "sms", recipient: c.phone!,
        provider_msg_id: r.msgId, delivery_status: r.ok ? "sent" : "failed", error: r.error,
      });
    }
  } else {
    // The tracker already texted the parent directly over 2G — no GPRS, no TLS,
    // no server in the path. Sending again would make one press look like two.
    await db.from("alerts").insert({
      sos_event_id: sos.id, channel: "sms", recipient: "skipped:device-sent",
    });
  }

  // RUNGS 2-4 become rows. A Vercel/Deno function cannot sleep for 300 s, and
  // rows survive a restart in a way a sleeping coroutine never did.
  const now = Date.now();
  await db.from("alert_queue").insert([
    { sos_event_id: sos.id, channel: "sms2",   due_at: new Date(now +  60_000).toISOString() },
    { sos_event_id: sos.id, channel: "voice",  due_at: new Date(now + 180_000).toISOString() },
    { sos_event_id: sos.id, channel: "voice2", due_at: new Date(now + 300_000).toISOString() },
  ]);
}

function json(b: unknown, status = 200) {
  return new Response(JSON.stringify(b), {
    status, headers: { "Content-Type": "application/json" },
  });
}
