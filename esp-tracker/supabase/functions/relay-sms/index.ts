// Scanner relay — receives SMS forwarded by the scanner's SIM900 modem.
//
// The scanner texts whatever it receives from the tracker's SIM800L to this
// endpoint. This function parses the SMS (LOC position report, WIFISCAN
// WiFi scan, or SOS alert), verifies the HMAC, and stores the data.
//
// Runs with the SERVICE ROLE key, so it bypasses RLS.
//
// Authenticated as the SCANNER (bearer token, same as ingest) — that proves
// WHICH scanner relayed this, not that the SMS body is genuine. The HMAC
// verification in sms-parser.ts proves a tracker actually composed this payload.

import { createClient } from "jsr:@supabase/supabase-js@2";
import { parseLoc, parseWifiScan } from "../_shared/sms-parser.ts";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

const SMS_CMD_SECRET = Deno.env.get("SMS_CMD_SECRET") ?? "";

const enc = new TextEncoder();
async function sha256(s: string) {
  const buf = await crypto.subtle.digest("SHA-256", enc.encode(s));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

const ts = (epoch: number) => new Date(epoch * 1000).toISOString();

// Parse SOS SMS format from tracker firmware:
//   "SOS from <device_id>. https://maps.google.com/?q=<lat>,<lon> (+/-<acc>m)"
//   "SOS from <device_id>. Position unknown, last known follows."
//   "SOS from <device_id>. Position unknown."
// The SCANNER-relayed copy (never the parent's — see sos.cpp) may carry a
// trailing " ID:<local-queue-id>" — the tracker's own store.cpp queue id.
// Optional and matched separately from the patterns above (not anchored to
// end of string) so it doesn't break parsing for a tracker not yet
// reflashed past its addition, or the rare case this SOMEHOW receives the
// parent's un-suffixed copy instead.
function parseSos(
  text: string,
): { deviceId: string; lat?: number; lon?: number; accuracy_m?: number; localId?: string } | null {
  const m = text.match(/^SOS from (.+?)\.\s/);
  if (!m) return null;
  const deviceId = m[1];
  const coordMatch = text.match(/q=(-?\d+\.?\d*),(-?\d+\.?\d*)\s*\(\+\/-(\d+)m\)/);
  const idMatch = text.match(/\sID:(\S+)\s*$/);
  const localId = idMatch ? idMatch[1] : undefined;
  if (coordMatch) {
    return {
      deviceId,
      lat: parseFloat(coordMatch[1]),
      lon: parseFloat(coordMatch[2]),
      accuracy_m: parseInt(coordMatch[3], 10),
      localId,
    };
  }
  return { deviceId, localId };
}

Deno.serve(async (req) => {
  const auth = req.headers.get("authorization") ?? "";
  if (!auth.startsWith("Bearer ")) return json({ error: "missing device token" }, 401);

  const { data: device } = await db.from("devices").select("*")
    .eq("token_hash", await sha256(auth.slice(7))).eq("active", true).maybeSingle();
  if (!device) return json({ error: "unknown device" }, 401);
  if (device.kind !== "scanner") return json({ error: "not a scanner" }, 403);

  const body = await req.json().catch(() => null);
  if (!body?.sender || !body?.text) return json({ error: "missing sender or text" }, 400);

  const now = new Date().toISOString();
  const receivedAt = body.received_at ?? now;

  // --- SOS format (sends directly to parent, scanner relays to Supabase) ---
  const sos = parseSos(body.text);
  if (sos) {
    // Look up the tracker by device_id from the SMS body
    const { data: tracker } = await db.from("devices").select("id,msisdn")
      .eq("id", sos.deviceId).eq("kind", "tracker").eq("active", true).maybeSingle();
    if (!tracker) return json({ ok: true, handled: false });

    // Idempotency key. When the tracker's own local queue id is present
    // (sos.localId — see sos.cpp/store.cpp), USE IT: the tracker retries an
    // unacked SOS with exponential backoff (store::drain(), "never gives
    // up"), and a retry is the same physical button press relayed twice,
    // not two separate emergencies. sos_events.event_id is UNIQUE, so a
    // retry with the same localId hits the insert's conflict path below
    // (sosEvent comes back null) and correctly skips starting a SECOND
    // escalation ladder — phone calls, repeat SMS — for one press.
    // Falls back to a timestamp-based id when localId is absent (a tracker
    // not yet reflashed past this addition), same as before this existed.
    const eventId = sos.localId ? `${tracker.id}-sos-${sos.localId}` : `${tracker.id}-sos-${Date.now()}`;

    // Store location if we have coordinates
    let locId: number | null = null;
    if (sos.lat != null && sos.lon != null) {
      const { data: loc } = await db.from("locations").upsert({
        event_id: `${eventId}-loc`, device_id: tracker.id,
        lat: sos.lat, lon: sos.lon,
        accuracy_m: sos.accuracy_m ?? 100, source: "gnss",
        recorded_at: now, received_at: receivedAt,
      }, { onConflict: "event_id", ignoreDuplicates: true }).select("id").maybeSingle();
      locId = loc?.id ?? null;
    }

    // Insert SOS event. onConflict is implicit via the unique constraint —
    // a duplicate event_id makes this a no-op insert; .maybeSingle() then
    // returns null, which the dedup check below relies on.
    const { data: sosEvent } = await db.from("sos_events").insert({
      event_id: eventId, device_id: tracker.id,
      triggered_at: now, received_at: receivedAt,
      latency_ms: 0,
      first_location_id: locId, best_location_id: locId,
      device_sms_sent: true,  // tracker sent SMS directly to parent
    }).select("id").maybeSingle();

    // Ack the tracker's local queue regardless of whether this was a fresh
    // event or a dedup'd retry — either way, THIS delivery reached the
    // server, which is exactly what the tracker's queue is waiting to hear
    // to stop retrying. Routed through the scanner's existing outbox relay
    // (relay.cpp already sends whatever outbox gives it to whatever
    // to_number is specified — no scanner firmware change needed here).
    // fallback_after is set far in the future deliberately: this is a
    // machine-only optimization message, not something worth spending real
    // SMS-provider money on via sweep-outbox if the scanner is briefly
    // offline — the tracker's own local backoff retry is a perfectly
    // adequate (free) fallback if this ack is delayed or lost.
    if (sos.localId && tracker.msisdn) {
      await db.from("outbox").insert({
        to_number: tracker.msisdn,
        body: `${SMS_CMD_SECRET} ACK ${sos.localId}`,
        child_device_id: tracker.id,
        fallback_after: new Date(Date.now() + 24 * 3600_000).toISOString(),
        ref: eventId,
      });
    }

    if (sosEvent) {
      // Rung 1: realtime push (implicit via Realtime publication)
      await db.from("alerts").insert({
        sos_event_id: sosEvent.id, channel: "realtime", recipient: "dashboard",
      });

      // Queue rungs 2-4 for dispatch cron
      const nowMs = Date.now();
      await db.from("alert_queue").insert([
        { sos_event_id: sosEvent.id, channel: "sms2",   due_at: new Date(nowMs + 60_000).toISOString() },
        { sos_event_id: sosEvent.id, channel: "voice",  due_at: new Date(nowMs + 180_000).toISOString() },
        { sos_event_id: sosEvent.id, channel: "voice2", due_at: new Date(nowMs + 300_000).toISOString() },
      ]);
    }

    await db.from("devices").update({ last_seen_at: receivedAt }).eq("id", tracker.id);
    return json({ ok: true, handled: true, type: "sos" });
  }

  // --- LOC format (routine position report) ---
  const loc = await parseLoc(body.text, SMS_CMD_SECRET);
  if (loc) {
    const { data: tracker } = await db.from("devices").select("id")
      .eq("msisdn", body.sender).eq("kind", "tracker").eq("active", true).maybeSingle();
    if (!tracker) return json({ ok: true, handled: false });

    const eventId = `${tracker.id}-${loc.recorded_at}`;
    const { error } = await db.from("locations").upsert({
      event_id: eventId, device_id: tracker.id,
      lat: loc.lat, lon: loc.lon, accuracy_m: loc.accuracy_m, source: loc.source,
      recorded_at: ts(loc.recorded_at), received_at: receivedAt,
    }, { onConflict: "event_id", ignoreDuplicates: true });

    if (error) console.error("[relay-sms] location upsert error:", error);

    // battery_pct rides on the LOC report — see report.cpp. undefined for a
    // tracker not yet reflashed past its addition to the wire format; don't
    // overwrite a real last-known value with null in that case.
    await db.from("devices").update({
      last_seen_at: receivedAt,
      ...(loc.battery_pct != null ? { battery_pct: loc.battery_pct } : {}),
    }).eq("id", tracker.id);
    return json({ ok: true, handled: true, type: "loc" });
  }

  // --- WIFISCAN format (WiFi BSSID scan for place matching) ---
  const wifi = await parseWifiScan(body.text, SMS_CMD_SECRET);
  if (wifi) {
    const { data: tracker } = await db.from("devices").select("id")
      .eq("msisdn", body.sender).eq("kind", "tracker").eq("active", true).maybeSingle();
    if (!tracker) return json({ ok: true, handled: false });

    const aps = await Promise.all(wifi.aps.map(async (a) => ({
      h: (await sha256(a.bssid.toLowerCase())).slice(0, 16),
      ssid: a.ssid.slice(0, 32),
      rssi: a.rssi,
    })));

    // Check known places
    let placeId: number | null = null;
    let placeLat: number | null = null;
    let placeLon: number | null = null;
    let placeSource = "wifi";
    let placeAccuracy = 40;

    const { data: places } = await db.from("places")
      .select("id,name,lat,lon,radius_m,bssid_set,ble_anchor_id")
      .eq("device_id", tracker.id).not("bssid_set", "is", null);

    const seenSet = new Set(aps.map((a) => a.h));
    for (const p of places ?? []) {
      const registered: string[] = p.bssid_set ?? [];
      if (!registered.length) continue;
      const inter = registered.filter((h) => seenSet.has(h)).length;
      if (inter >= Math.max(2, Math.floor(registered.length / 3))) {
        placeId = p.id;
        placeLat = p.lat;
        placeLon = p.lon;
        placeSource = p.ble_anchor_id ? "ble_anchor" : "wifi";
        placeAccuracy = p.radius_m ?? 40;
        break;
      }
    }

    await db.from("wifi_scans").insert({
      device_id: tracker.id, recorded_at: ts(wifi.recorded_at), place_id: placeId, aps,
    });

    if (placeId && placeLat != null && placeLon != null) {
      const eventId = `${tracker.id}-wifi-${wifi.recorded_at}`;
      await db.from("locations").upsert({
        event_id: eventId, device_id: tracker.id,
        lat: placeLat, lon: placeLon, accuracy_m: placeAccuracy,
        source: placeSource, recorded_at: ts(wifi.recorded_at),
        received_at: receivedAt, place_id: placeId,
      }, { onConflict: "event_id", ignoreDuplicates: true });
    }

    await db.from("devices").update({ last_seen_at: receivedAt }).eq("id", tracker.id);
    return json({ ok: true, handled: true, type: "wifiscan" });
  }

  // Unknown format — accepted but not handled
  return json({ ok: true, handled: false });
});

function json(b: unknown, status = 200) {
  return new Response(JSON.stringify(b), {
    status, headers: { "Content-Type": "application/json" },
  });
}
