// Scanner relay — receives SMS forwarded by the scanner's SIM900 modem.
//
// The scanner texts whatever it receives from the tracker's SIM800L to this
// endpoint. This function parses the SMS (LOC position report or WIFISCAN
// WiFi scan), verifies the HMAC, and stores the data.
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

  // Try LOC format first
  const loc = await parseLoc(body.text, SMS_CMD_SECRET);
  if (loc) {
    // Look up the tracker by phone number
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

    await db.from("devices").update({ last_seen_at: receivedAt }).eq("id", tracker.id);
    return json({ ok: true, handled: true });
  }

  // Try WIFISCAN format
  const wifi = await parseWifiScan(body.text, SMS_CMD_SECRET);
  if (wifi) {
    const { data: tracker } = await db.from("devices").select("id")
      .eq("msisdn", body.sender).eq("kind", "tracker").eq("active", true).maybeSingle();
    if (!tracker) return json({ ok: true, handled: false });

    // Hash BSSIDs and try to match a known place
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

    // Store the WiFi scan
    await db.from("wifi_scans").insert({
      device_id: tracker.id, recorded_at: ts(wifi.recorded_at), place_id: placeId, aps,
    });

    // If a place matched, also store a location entry
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
    return json({ ok: true, handled: true });
  }

  // Unknown format — accepted but not handled
  return json({ ok: true, handled: false });
});

function json(b: unknown, status = 200) {
  return new Response(JSON.stringify(b), {
    status, headers: { "Content-Type": "application/json" },
  });
}
