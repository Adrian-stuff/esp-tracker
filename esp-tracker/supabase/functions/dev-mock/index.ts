// Presentation/demo tool — mock a tracker's location trail and SOS events
// without any real hardware. Hidden, not linked from the dashboard: reached
// only by knowing the URL (web/dev.html) and a shared secret, not tied to
// any real device's bearer token or any user's device_access grants — a
// presenter needs to drive ANY demo device regardless of which dashboard
// account is signed in.
//
// SAFETY: mock SOS events are marked is_drill=true and NEVER populate
// alert_queue (the escalation ladder — see supabase/functions/dispatch).
// dispatch does not currently check is_drill at all, so the only way to
// guarantee a demo never triggers a real SMS/voice call to a real parent's
// phone is to simply never queue those rungs from here in the first place.
// Only a "realtime" alert row is inserted, which is what makes the
// dashboard's SOS banner appear — exactly the demo-visible effect wanted,
// with none of the real-world side effects.
//
// The ONE deliberate exception is the notify-parent action (see its own
// comment below): it queues a real SMS, sent by a real tracker's own
// SIM800L, to a phone number the presenter must type in explicitly every
// time — never looked up from real device_access data, so a mock location
// can never silently text a real parent.
//
// Auth: Authorization: Bearer <DEV_MOCK_SECRET> — set with
// `supabase secrets set DEV_MOCK_SECRET=...`. Not a device token, not a
// user JWT: this endpoint is a presentation control surface, not a device
// or a user, and is scoped by knowing the secret, nothing else.

import { createClient } from "jsr:@supabase/supabase-js@2";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

const DEV_MOCK_SECRET = Deno.env.get("DEV_MOCK_SECRET") ?? "";

const enc = new TextEncoder();
async function sha256(s: string) {
  const buf = await crypto.subtle.digest("SHA-256", enc.encode(s));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
async function bssidHash(bssid: string) {
  return (await sha256(bssid.toLowerCase())).slice(0, 16);
}

// This is the one function in this project called DIRECTLY from a browser
// with a JSON body (every other browser-facing function — claim, locate —
// happens to work without this, which just means they've never actually
// been exercised from a cross-origin/preflighted context that matters;
// dev-mock's own testing surfaced a real CORS preflight failure, so it
// gets explicit handling rather than assuming platform defaults cover it).
const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, content-type",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
};

function json(b: unknown, status = 200) {
  return new Response(JSON.stringify(b), {
    status, headers: { "Content-Type": "application/json", ...CORS_HEADERS },
  });
}

// Ensures the mock device row exists so locations/sos_events' FK
// constraints are satisfiable, WITHOUT ever touching a real device's real
// token_hash if device_id happens to collide with one already registered.
//
// tracker.html renders `child_name || name` as the device's on-screen
// title — defaulting either to the raw device_id (e.g. "demo-tracker-01")
// is exactly what gives a mock device away to an audience at a glance. If
// this device already exists (created by an earlier dev-mock call, so its
// token_hash is guaranteed to start with "dev-mock-", never a real device's
// real hash), a later call that finally supplies a childName updates the
// display name in place — the presenter can name it any time, not only on
// first use, without deleting and recreating the device.
async function ensureDevice(deviceId: string, childName?: string) {
  const { data: existing } = await db.from("devices").select("id,token_hash").eq("id", deviceId).maybeSingle();
  if (existing) {
    if (childName && existing.token_hash?.startsWith("dev-mock-")) {
      await db.from("devices").update({ name: childName, child_name: childName }).eq("id", deviceId);
    }
    return;
  }
  const label = childName || "Tracker";
  await db.from("devices").insert({
    id: deviceId, kind: "tracker", name: label,
    child_name: childName || label,
    token_hash: `dev-mock-${deviceId}`,   // never a valid bearer token — see ingest's sha256 check
    active: true,
  });
}

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response(null, { headers: CORS_HEADERS });

  if (!DEV_MOCK_SECRET) return json({ error: "DEV_MOCK_SECRET not configured on the server" }, 500);
  const auth = req.headers.get("authorization") ?? "";
  if (auth !== `Bearer ${DEV_MOCK_SECRET}`) return json({ error: "unauthorized" }, 401);

  const body = await req.json().catch(() => null);
  if (!body?.action) return json({ error: "missing action" }, 400);
  const now = new Date().toISOString();

  // ------------------------------------------------------ notify-parent ----
  // Queues a real SMS to a real phone, sent by the REAL tracker's own
  // SIM800L, describing a mock location — the one dev-mock action that
  // deliberately reaches outside Supabase into the physical world. Reuses
  // the outbox table byte-for-byte (see migrations/0005_outbox.sql and
  // functions/outbox/index.ts): a row here is indistinguishable from a
  // real tap-notification row to the device that polls and relays it.
  //
  // THIS ONLY WORKS if a real tracker whose DEVICE_ID (config.h) equals
  // device_id is powered on, has real WiFi credentials configured (not
  // "change-me"), and is currently online — outbox/index.ts's claim()
  // filters strictly by child_device_id == the polling device's own id,
  // so a device_id that doesn't match any currently-polling hardware just
  // sits in outbox until fallback_after and is never delivered. That is
  // the correct, safe behavior (no accidental delivery to who a
  // mismatched id might belong to) — not a bug to route around.
  //
  // `to` is REQUIRED and never looked up from device_access: this is a
  // presentation control surface with no user context, and silently
  // pulling a real parent's saved phone number for a MOCK location would
  // be exactly the kind of accidental-real-side-effect this file's other
  // two actions (sos, resolve) go out of their way to avoid.
  if (body.action === "notify-parent") {
    const { device_id, to, lat, lon } = body;
    if (!device_id || !to || lat == null || lon == null) {
      return json({ error: "device_id, to, lat, lon required" }, 400);
    }
    const childLabel = body.child_name || device_id;
    const mapsUrl = `https://maps.google.com/?q=${lat},${lon}`;
    const smsBody = `${childLabel} location (DEMO, not a real report): ${mapsUrl}`;
    const fallbackAfter = new Date(Date.now() + 10 * 60 * 1000).toISOString();

    const { data: row, error } = await db.from("outbox").insert({
      to_number: to, body: smsBody, child_device_id: device_id,
      fallback_after: fallbackAfter,
    }).select("id").maybeSingle();
    if (error || !row) return json({ error: error?.message ?? "insert failed" }, 500);
    return json({ ok: true, outbox_id: row.id, body: smsBody });
  }

  // ---------------------------------------------------------- location ----
  if (body.action === "location") {
    const { device_id, lat, lon } = body;
    if (!device_id || lat == null || lon == null) return json({ error: "device_id, lat, lon required" }, 400);
    await ensureDevice(device_id, body.child_name);

    const eventId = `${device_id}-dev-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
    const recordedAt = body.recorded_at ? new Date(body.recorded_at).toISOString() : now;
    await db.from("locations").insert({
      event_id: eventId, device_id,
      lat, lon, accuracy_m: body.accuracy_m ?? 12, source: body.source ?? "gnss",
      recorded_at: recordedAt, received_at: now,
      battery_pct: body.battery_pct ?? null, speed_mps: body.speed_mps ?? null,
    });
    await db.from("devices").update({
      last_seen_at: now,
      ...(body.battery_pct != null ? { battery_pct: body.battery_pct } : {}),
    }).eq("id", device_id);
    return json({ ok: true, event_id: eventId });
  }

  // --------------------------------------------------------------- sos ----
  if (body.action === "sos") {
    const { device_id } = body;
    if (!device_id) return json({ error: "device_id required" }, 400);
    await ensureDevice(device_id, body.child_name);

    let locId: number | null = null;
    if (body.lat != null && body.lon != null) {
      const { data: loc } = await db.from("locations").insert({
        event_id: `${device_id}-dev-sos-loc-${Date.now()}`, device_id,
        lat: body.lat, lon: body.lon, accuracy_m: body.accuracy_m ?? 12,
        source: body.source ?? "gnss", recorded_at: now, received_at: now,
      }).select("id").maybeSingle();
      locId = loc?.id ?? null;
    }

    const eventId = `${device_id}-dev-sos-${Date.now()}`;
    const { data: sos, error } = await db.from("sos_events").insert({
      event_id: eventId, device_id, triggered_at: now, received_at: now,
      latency_ms: 0, first_location_id: locId, best_location_id: locId,
      device_sms_sent: true,   // never real — see file header; this just
                                // tells the dashboard/dispatch this SOS's
                                // "device already texted directly" path is
                                // considered handled, consistent with how
                                // a real SOS marks it.
      is_drill: true,
      notes: "Mocked from /dev for a presentation — not a real event.",
    }).select("id").maybeSingle();
    if (error || !sos) return json({ error: error?.message ?? "insert failed" }, 500);

    // Realtime-only: makes the dashboard's SOS banner appear. Deliberately
    // NOT inserting into alert_queue — see file header on why a demo must
    // never reach dispatch's real SMS/voice escalation.
    await db.from("alerts").insert({
      sos_event_id: sos.id, channel: "realtime", recipient: "dashboard (mock)",
    });
    return json({ ok: true, event_id: eventId, sos_id: sos.id });
  }

  // ------------------------------------------------------------ resolve ----
  if (body.action === "resolve") {
    const { event_id } = body;
    if (!event_id) return json({ error: "event_id required" }, 400);
    await db.from("sos_events").update({
      status: "resolved", resolved_at: now,
    }).eq("event_id", event_id);
    return json({ ok: true });
  }

  // ----------------------------------------------------------- place ----
  // CRUD for WiFi places. Each place stores a set of BSSID hashes that
  // identify it — the same hashing the tracker firmware and relay-sms use
  // (sha256, first 16 hex chars). The demo presenter defines places once,
  // then the wifiscan action replays them to trigger place matching.
  if (body.action === "place") {
    const { device_id } = body;
    if (!device_id) return json({ error: "device_id required" }, 400);
    await ensureDevice(device_id);

    // --- list ---
    if (body.sub === "list" || (!body.sub && !body.name)) {
      const { data: places } = await db.from("places")
        .select("id,name,kind,bssid_set,lat,lon,radius_m")
        .eq("device_id", device_id).order("name");
      return json({ ok: true, places: places ?? [] });
    }

    // --- create ---
    if (body.sub === "create") {
      if (!body.name || !body.bssids?.length) {
        return json({ error: "name and bssids[] required" }, 400);
      }
      const hashes = await Promise.all(body.bssids.map(bssidHash));
      const { data: place, error } = await db.from("places").insert({
        device_id, name: body.name,
        kind: body.kind ?? "other",
        bssid_set: hashes,
        lat: body.lat ?? null, lon: body.lon ?? null,
        radius_m: body.radius_m ?? 40,
      }).select("id,name,kind,bssid_set,lat,lon,radius_m").maybeSingle();
      if (error) return json({ error: error.message }, 500);
      return json({ ok: true, place });
    }

    // --- delete ---
    if (body.sub === "delete") {
      if (!body.place_id) return json({ error: "place_id required" }, 400);
      await db.from("places").delete().eq("id", body.place_id).eq("device_id", device_id);
      return json({ ok: true });
    }

    return json({ error: "unknown place sub-action (list/create/delete)" }, 400);
  }

  // --------------------------------------------------------- wifiscan ----
  // Mock a WiFi scan: inserts into wifi_scans, runs place matching, and
  // optionally inserts a location if a place matched. This is what makes
  // the dashboard show "at school" / "at home" during a demo — the same
  // path a real tracker's WIFISCAN SMS would take, but without hardware.
  if (body.action === "wifiscan") {
    const { device_id, aps } = body;
    if (!device_id || !aps?.length) {
      return json({ error: "device_id and aps[] required (each: {bssid, ssid, rssi})" }, 400);
    }
    await ensureDevice(device_id);

    const recordedAt = body.recorded_at ? new Date(body.recorded_at).toISOString() : now;
    const hashedAps = await Promise.all(aps.map(async (a: { bssid: string; ssid?: string; rssi?: number }) => ({
      h: await bssidHash(a.bssid),
      ssid: (a.ssid ?? "").slice(0, 32),
      rssi: a.rssi ?? -50,
    })));

    // Place matching — same logic as ingest and relay-sms
    const { data: places } = await db.from("places")
      .select("id,name,lat,lon,radius_m,bssid_set,ble_anchor_id")
      .eq("device_id", device_id).not("bssid_set", "is", null);

    const seen = new Set(hashedAps.map((a) => a.h));
    let placeId: number | null = null;
    let placeLat: number | null = null;
    let placeLon: number | null = null;
    let placeSource = "wifi";
    let placeAccuracy = 40;
    let matchedPlace: string | null = null;

    for (const p of places ?? []) {
      const registered: string[] = p.bssid_set ?? [];
      if (!registered.length) continue;
      const inter = registered.filter((h) => seen.has(h)).length;
      if (inter >= Math.max(2, Math.floor(registered.length / 3))) {
        placeId = p.id;
        placeLat = p.lat;
        placeLon = p.lon;
        placeSource = p.ble_anchor_id ? "ble_anchor" : "wifi";
        placeAccuracy = p.radius_m ?? 40;
        matchedPlace = p.name;
        break;
      }
    }

    // Insert the scan record
    await db.from("wifi_scans").insert({
      device_id, recorded_at: recordedAt, place_id: placeId, aps: hashedAps,
    });

    // If a place matched and has coordinates, insert a location
    let locationEventId: string | null = null;
    if (placeId && placeLat != null && placeLon != null) {
      locationEventId = `${device_id}-dev-wifi-${Date.now()}`;
      await db.from("locations").upsert({
        event_id: locationEventId, device_id,
        lat: placeLat, lon: placeLon, accuracy_m: placeAccuracy,
        source: placeSource, recorded_at: recordedAt, received_at: now,
        place_id: placeId,
      }, { onConflict: "event_id", ignoreDuplicates: true });
      await db.from("devices").update({ last_seen_at: now }).eq("id", device_id);
    }

    return json({
      ok: true, scanned: hashedAps.length,
      matched_place: matchedPlace, place_id: placeId,
      location_event_id: locationEventId,
    });
  }

  return json({ error: `unknown action: ${body.action}` }, 400);
});
