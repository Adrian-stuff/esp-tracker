// Parent dashboard — Supabase edition.
//
// Talks to Postgres directly through PostgREST with the anon key. Every query
// below is filtered by RLS, so "select * from locations" returns only the
// devices this parent was granted. That is the whole security model: if RLS is
// off, this page is an open door.
//
// The rule this file exists to enforce: a position is never drawn without its
// source, its accuracy and its age. A 12 m GNSS fix and a 2 km cell estimate
// are both "a pin" if you let them be.

const sb = supabase.createClient(window.SUPABASE_URL, window.SUPABASE_ANON_KEY);

const STALE_S = 15 * 60;
const SOURCE = {
  gnss:       { label: "GPS",                color: "#0b6e68" },
  wifi:       { label: "Wi-Fi",              color: "#2f6fb0" },
  ble_anchor: { label: "at a known place",   color: "#6b3fa0" },
  cell:       { label: "cell tower",         color: "#8a6212" },
  manual:     { label: "manual",             color: "#6d7b8b" },
};

const $ = (id) => document.getElementById(id);
let map, markers = {}, fitted = false, activeSos = null, primaryDevice = null;
let trailLine = null, trailDevice = null;
let sosAudioCtx = null, sosBeepOsc = null, sosBeepInterval = null;

const secsAgo = (iso) => Math.max(0, (Date.now() - new Date(iso).getTime()) / 1000);
function ago(s) {
  if (s == null) return "never";
  if (s < 60) return `${s | 0}s ago`;
  if (s < 3600) return `${(s / 60) | 0} min ago`;
  if (s < 86400) return `${(s / 3600) | 0} hr ago`;
  return `${(s / 86400) | 0} days ago`;
}

// ---------------------------------------------------------------- auth -----
async function boot() {
  const { data: { session } } = await sb.auth.getSession();
  if (!session) { $("signin").hidden = false; return; }
  $("signin").hidden = true;
  initMap();
  await refresh();
  subscribe();
  setInterval(refresh, 60000);   // realtime carries the news; this is a safety net
}

let signupMode = false;
$("signin-toggle").addEventListener("click", () => {
  signupMode = !signupMode;
  $("signin-heading").textContent = signupMode ? "Create account" : "Tracker";
  $("signin-submit").textContent = signupMode ? "Create account" : "Sign in";
  $("signin-toggle").textContent = signupMode
    ? "Already have an account? Sign in" : "Need an account? Create one";
  $("password2").hidden = !signupMode;
  $("password2").required = signupMode;
  $("signin-error").textContent = "";
  $("signin-note").hidden = true;
});

$("signin-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  $("signin-error").textContent = "";
  $("signin-note").hidden = true;

  if (signupMode) {
    if ($("password").value !== $("password2").value) {
      $("signin-error").textContent = "Passwords don't match.";
      return;
    }
    const { data, error } = await sb.auth.signUp({
      email: $("email").value, password: $("password").value,
    });
    if (error) { $("signin-error").textContent = error.message; return; }
    // A fresh account has zero device_access rows, so RLS shows it nothing —
    // creating an account grants no visibility into anyone's data by itself.
    // The claim code (below, once signed in) is the only way that changes.
    if (data.session) { location.reload(); return; }
    $("signin-note").hidden = false;
    $("signin-note").textContent = "Check your email to confirm the account, then sign in.";
    return;
  }

  const { error } = await sb.auth.signInWithPassword({
    email: $("email").value, password: $("password").value,
  });
  if (error) { $("signin-error").textContent = error.message; return; }
  location.reload();
});
$("signout").addEventListener("click", async () => {
  await sb.auth.signOut(); location.reload();
});

// ------------------------------------------------------------- pairing -----
// The one write a signed-in user can make outside RLS's normal read/write
// policies: the claim Edge Function checks the code with the service role
// and inserts device_access itself. See supabase/functions/claim.
$("claim-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const btn = e.submitter;
  const status = $("claim-status");
  status.textContent = ""; status.className = "hint";
  btn.disabled = true;

  const { data: { session } } = await sb.auth.getSession();
  const r = await fetch(`${window.SUPABASE_URL}/functions/v1/claim`, {
    method: "POST",
    headers: { "Content-Type": "application/json",
               Authorization: `Bearer ${session.access_token}` },
    body: JSON.stringify({ code: $("claim-code").value.trim(), phone: $("claim-phone").value.trim() }),
  });
  const body = await r.json().catch(() => ({}));
  btn.disabled = false;

  if (!r.ok) { status.textContent = body.error || "Couldn't add that device."; status.className = "hint err"; return; }
  status.textContent = `Added ${body.device.child_name || body.device.name}.`;
  $("claim-form").reset();
  refresh();
});

// ----------------------------------------------------------------- map -----
function initMap() {
  map = L.map("map").setView([14.5995, 120.9842], 12);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 19,
    attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
  }).addTo(map);
}

function draw(d) {
  const loc = d.location;
  if (!loc || !map) return;
  const meta = SOURCE[loc.source] || SOURCE.manual;
  const stale = loc.age_s > STALE_S;

  if (!markers[d.id]) {
    markers[d.id] = {
      // Sized from the REAL accuracy, so a cell fix looks like the
      // kilometre-wide guess it is instead of a confident dot.
      ring: L.circle([loc.lat, loc.lon], { radius: loc.accuracy_m, weight: 1 }).addTo(map),
      dot:  L.circleMarker([loc.lat, loc.lon], { radius: 8, weight: 3 }).addTo(map),
    };
  }
  const m = markers[d.id];
  m.ring.setLatLng([loc.lat, loc.lon]).setRadius(loc.accuracy_m)
        .setStyle({ color: meta.color, fillOpacity: stale ? .05 : .12, opacity: stale ? .35 : .7 });
  m.dot.setLatLng([loc.lat, loc.lon])
       .setStyle({ color: meta.color, fillColor: meta.color,
                   fillOpacity: stale ? .35 : 1, opacity: stale ? .45 : 1 });
  m.dot.bindPopup(
    `<b>${d.child_name || d.name}</b><br>${loc.place ? `<b>At ${loc.place}</b><br>` : ""}` +
    `<span class="src">${meta.label} &middot; &plusmn;${Math.round(loc.accuracy_m)} m ` +
    `&middot; ${ago(loc.age_s)}</span>`);

  if (!fitted) { map.setView([loc.lat, loc.lon], 16); fitted = true; }
}

// ---------------------------------------------------------------- data -----
async function refresh() {
  const { data: devices } = await sb.from("devices").select("*").eq("active", true);
  if (!devices) return;

  const enriched = [];
  for (const d of devices.filter((x) => x.kind === "tracker")) {
    const { data: loc } = await sb.from("locations")
      .select("lat,lon,accuracy_m,source,recorded_at,places(name)")
      .eq("device_id", d.id).order("recorded_at", { ascending: false }).limit(1).maybeSingle();
    enriched.push({
      ...d,
      location: loc ? {
        lat: loc.lat, lon: loc.lon, accuracy_m: loc.accuracy_m, source: loc.source,
        place: loc.places?.name ?? null, age_s: secsAgo(loc.recorded_at),
      } : null,
    });
  }
  renderDevices(enriched);

  // Scanners
  renderScanners(devices.filter(x => x.kind === "scanner"));

  // Attendance: last 200 taps for days view, last 8 for recent
  const { data: taps } = await sb.from("attendance")
    .select("card_uid,direction,recorded_at,cards(child_name)")
    .order("recorded_at", { ascending: false }).limit(200);
  const allTaps = taps ?? [];
  renderAttendance(allTaps.slice(0, 8));
  renderAttendanceDays(allTaps);

  const { data: open } = await sb.from("sos_events")
    .select("event_id,device_id,status,locations!sos_events_best_location_id_fkey(lat,lon,accuracy_m,source)")
    .eq("status", "open").order("triggered_at", { ascending: false }).limit(1).maybeSingle();
  if (open && !activeSos) showSos(open);
}

function renderDevices(list) {
  const box = $("devices");
  box.innerHTML = "";
  list.forEach((d) => {
    const loc = d.location;
    const src = loc ? (SOURCE[loc.source] || SOURCE.manual) : null;
    const stale = loc && loc.age_s > STALE_S;
    const el = document.createElement("div");
    el.className = "dev" + (stale ? " stale" : "");
    el.innerHTML = `
      <div class="top">
        <span class="name">${d.child_name || d.name}</span>
        <span class="age">${loc ? ago(loc.age_s) : "no fix yet"}</span>
      </div>
      ${loc?.place ? `<div class="place">At ${loc.place}</div>` : ""}
      <div class="meta">
        <span><b>${d.battery_pct ?? "—"}${d.battery_pct != null ? "%" : ""}</b> battery</span>
        ${loc ? `<span>${src.label} &plusmn;${Math.round(loc.accuracy_m)}m</span>` : ""}
        ${d.signal_csq != null ? `<span>CSQ ${d.signal_csq}</span>` : ""}
      </div>
      ${d.balance_pesos != null && d.balance_pesos < 20
        ? `<div class="warn">Load is low (₱${d.balance_pesos}) — top up or the tracker goes silent.</div>` : ""}
      ${d.battery_pct != null && d.battery_pct <= 20
        ? `<div class="warn">Battery ${d.battery_pct}% — charge tonight.</div>` : ""}
      <div style="display:flex; gap:6px; flex-wrap:wrap">
        <button data-locate="${d.id}">Locate now</button>
        <button class="trail-btn" data-trail="${d.id}" onclick="toggleTrail('${d.id}', this)">Show trail</button>
      </div>`;
    box.appendChild(el);
    if (loc) draw(d);
    if (!primaryDevice) { primaryDevice = d.id; loadPlaces(d.id); }
  });

  box.querySelectorAll("[data-locate]").forEach((b) =>
    b.addEventListener("click", async () => {
      b.disabled = true; b.textContent = "Waking the tracker…";
      const { data: { session } } = await sb.auth.getSession();
      const r = await fetch(`${window.SUPABASE_URL}/functions/v1/locate`, {
        method: "POST",
        headers: { "Content-Type": "application/json",
                   Authorization: `Bearer ${session.access_token}` },
        body: JSON.stringify({ device_id: b.dataset.locate }),
      });
      b.textContent = r.ok ? "Asked — expect a fix in ~20s" : "Couldn't reach the tracker";
      setTimeout(() => { b.disabled = false; b.textContent = "Locate now"; }, 25000);
    }));
}

function renderAttendance(rows) {
  const ul = $("attendance");
  if (!rows.length) { ul.innerHTML = '<li class="muted">No taps yet</li>'; return; }
  ul.innerHTML = "";
  rows.forEach((r) => {
    const li = document.createElement("li");
    const t = new Date(r.recorded_at);
    li.innerHTML = `<span>${r.cards?.child_name || r.card_uid} tapped ${r.direction}</span>` +
      `<time>${t.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}</time>`;
    ul.appendChild(li);
  });
}

// -------------------------------------------------------------- places -----
async function loadPlaces(deviceId) {
  const { data: places } = await sb.from("places").select("*").eq("device_id", deviceId);
  const named = {};
  (places ?? []).forEach((p) => (p.bssid_set ?? []).forEach((h) => (named[h] = p.name)));

  const box = $("places");
  box.innerHTML = places?.length ? "" :
    '<p class="muted">No places yet. Name one below so the map can say where your child is, not just where.</p>';
  (places ?? []).forEach((p) => {
    const row = document.createElement("div");
    row.className = "place-row";
    row.innerHTML = `<span class="pname">${p.name}</span>` +
      `<span class="pmeta">${(p.bssid_set ?? []).length} networks</span>` +
      `<button data-del="${p.id}">Remove</button>`;
    box.appendChild(row);
  });
  box.querySelectorAll("[data-del]").forEach((b) =>
    b.addEventListener("click", async () => {
      await sb.from("places").delete().eq("id", b.dataset.del);
      loadPlaces(deviceId);
    }));

  // What the tracker has actually seen this week. An AP it sees constantly is
  // somewhere the child spends time — which is exactly what is worth naming.
  const since = new Date(Date.now() - 7 * 86400e3).toISOString();
  const { data: scans } = await sb.from("wifi_scans")
    .select("aps,recorded_at").eq("device_id", deviceId)
    .gte("recorded_at", since).order("recorded_at", { ascending: false }).limit(300);

  const seen = {};
  (scans ?? []).forEach((s) => (s.aps ?? []).forEach((a) => {
    const e = seen[a.h] ??= { h: a.h, ssid: a.ssid || "", n: 0, rssi: a.rssi };
    e.n++; if (a.ssid) e.ssid = a.ssid;
  }));
  const rows = Object.values(seen).sort((a, b) => b.n - a.n).slice(0, 40);

  const list = $("ap-list");
  if (!rows.length) {
    list.innerHTML = '<p class="muted">No Wi-Fi scans yet. They arrive with the next report.</p>';
    return;
  }
  list.innerHTML = "";
  rows.forEach((a) => {
    const el = document.createElement("label");
    el.className = "ap";
    el.innerHTML =
      `<input type="checkbox" value="${a.h}" ${named[a.h] ? "disabled" : ""}>` +
      `<span class="ssid ${a.ssid ? "" : "unnamed"}">${a.ssid || "(hidden network)"}</span>` +
      `<span class="bars">${named[a.h] ? `<span class="taken">${named[a.h]}</span>` : ""}` +
      `<span>seen ${a.n}&times;</span></span>`;
    list.appendChild(el);
  });
}

$("place-save").addEventListener("click", async () => {
  const btn = $("place-save");
  const name = $("place-name").value.trim();
  const picked = [...document.querySelectorAll("#ap-list input:checked")].map((i) => i.value);
  if (!name || !picked.length) {
    btn.textContent = !name ? "Name it first" : "Tick some networks";
    setTimeout(() => (btn.textContent = "Save place"), 2000); return;
  }
  btn.disabled = true;
  await sb.from("places").insert({
    device_id: primaryDevice, name, kind: $("place-kind").value, bssid_set: picked,
  });
  $("place-name").value = "";
  btn.disabled = false; btn.textContent = "Saved";
  setTimeout(() => (btn.textContent = "Save place"), 1500);
  loadPlaces(primaryDevice);
});

// ----------------------------------------------------------------- sos -----
function showSos(row) {
  activeSos = row.event_id;
  const loc = row.locations;
  $("sos-title").textContent = "SOS — button pressed";
  $("sos-detail").textContent = loc
    ? `±${Math.round(loc.accuracy_m)} m via ${(SOURCE[loc.source] || SOURCE.manual).label}`
    : "Position not yet known.";
  $("sos-banner").hidden = false;
  startBeep();
  if (loc && map) map.setView([loc.lat, loc.lon], 17);
}

$("sos-ack").addEventListener("click", async () => {
  if (!activeSos) return;
  stopBeep();
  const { data: { user } } = await sb.auth.getUser();
  // Setting status to 'acknowledged' IS the cut: the dispatch cron skips every
  // remaining rung on its next pass, so the voice calls never happen.
  await sb.from("sos_events").update({
    status: "acknowledged", acknowledged_by: user.id,
    acknowledged_at: new Date().toISOString(),
  }).eq("event_id", activeSos);
  $("sos-banner").hidden = true;
  activeSos = null;
});

// ------------------------------------------------------------- realtime ----
// Replaces the hand-rolled WebSocket hub. RLS still applies to the stream, so a
// parent is only ever pushed rows for their own devices.
function subscribe() {
  sb.channel("live")
    .on("postgres_changes", { event: "INSERT", schema: "public", table: "sos_events" },
        (p) => showSos(p.new))
    .on("postgres_changes", { event: "UPDATE", schema: "public", table: "sos_events" },
        (p) => { if (p.new.status !== "open") { $("sos-banner").hidden = true; activeSos = null; stopBeep(); } })
    .on("postgres_changes", { event: "INSERT", schema: "public", table: "locations" },
        () => refresh())
    .on("postgres_changes", { event: "INSERT", schema: "public", table: "attendance" },
        () => refresh())
    .subscribe((status) => {
      const live = status === "SUBSCRIBED";
      $("conn").textContent = live ? "live" : "reconnecting…";
      $("conn").className = "chip " + (live ? "live" : "down");
    });
}

// --------------------------------------------------------------- beep -----
function startBeep() {
  try {
    sosAudioCtx = new (window.AudioContext || window.webkitAudioContext)();
    sosBeepOsc = sosAudioCtx.createOscillator();
    const gain = sosAudioCtx.createGain();
    sosBeepOsc.type = "square";
    sosBeepOsc.frequency.value = 440;
    gain.gain.value = 0.3;
    sosBeepOsc.connect(gain);
    gain.connect(sosAudioCtx.destination);
    sosBeepOsc.start();
    // Pulse: on 200ms, off 200ms
    let on = true;
    sosBeepInterval = setInterval(() => {
      gain.gain.value = on ? 0.3 : 0;
      on = !on;
    }, 200);
  } catch {}
}

function stopBeep() {
  if (sosBeepInterval) { clearInterval(sosBeepInterval); sosBeepInterval = null; }
  if (sosBeepOsc) { try { sosBeepOsc.stop(); } catch {} sosBeepOsc = null; }
  if (sosAudioCtx) { try { sosAudioCtx.close(); } catch {} sosAudioCtx = null; }
}

// ------------------------------------------------------------- trail -----
async function toggleTrail(deviceId, btn) {
  if (trailDevice === deviceId) {
    // Hide trail
    if (trailLine) { map.removeLayer(trailLine); trailLine = null; }
    trailDevice = null;
    btn.classList.remove("active");
    btn.textContent = "Show trail";
    return;
  }
  // Show trail for this device
  btn.disabled = true;
  btn.textContent = "Loading…";
  const since = new Date(Date.now() - 24 * 3600e3).toISOString();
  const { data: pts } = await sb.from("locations")
    .select("lat,lon,source,recorded_at")
    .eq("device_id", deviceId)
    .gte("recorded_at", since)
    .order("recorded_at", { ascending: true })
    .limit(500);
  btn.disabled = false;

  if (!pts || pts.length < 2) {
    btn.textContent = "No trail yet";
    setTimeout(() => { btn.textContent = "Show trail"; }, 2000);
    return;
  }

  // Remove old trail
  if (trailLine) { map.removeLayer(trailLine); trailLine = null; }

  // Draw polyline colored by source (use the most common source for the line)
  const coords = pts.map(p => [p.lat, p.lon]);
  const mainSource = pts[Math.floor(pts.length / 2)].source;
  const meta = SOURCE[mainSource] || SOURCE.manual;
  trailLine = L.polyline(coords, {
    color: meta.color, weight: 3, opacity: 0.7, dashArray: "6 4",
  }).addTo(map);

  // Mark start and end
  L.circleMarker(coords[0], { radius: 5, color: meta.color, fillColor: "#fff", fillOpacity: 1, weight: 2 }).addTo(map)
    .bindPopup(`<b>Start</b><br>${new Date(pts[0].recorded_at).toLocaleTimeString()}`);
  L.circleMarker(coords[coords.length - 1], { radius: 5, color: meta.color, fillColor: meta.color, fillOpacity: 1, weight: 2 }).addTo(map)
    .bindPopup(`<b>Now</b><br>${new Date(pts[pts.length - 1].recorded_at).toLocaleTimeString()}`);

  trailDevice = deviceId;
  // Update all trail buttons
  document.querySelectorAll(".trail-btn").forEach(b => {
    b.classList.toggle("active", b.dataset.trail === deviceId);
    if (b.dataset.trail !== deviceId) b.textContent = "Show trail";
  });
  btn.textContent = "Hide trail";
  btn.classList.add("active");
  map.fitBounds(trailLine.getBounds(), { padding: [40, 40] });
}

// ----------------------------------------------------------- scanners -----
function renderScanners(list) {
  const box = $("scanners");
  if (!list.length) { box.innerHTML = '<p class="muted">No gate stations registered.</p>'; return; }
  box.innerHTML = "";
  list.forEach((s) => {
    const age = s.last_seen_at ? secsAgo(s.last_seen_at) : null;
    const online = age != null && age < 5 * 60;
    const el = document.createElement("div");
    el.className = "scanner";
    el.innerHTML = `
      <span class="sname">${s.name || s.id}</span>
      <span class="sstatus ${online ? "on" : "off"}">${online ? "online" : ago(age) + (age != null ? "" : " — never seen")}</span>`;
    box.appendChild(el);
  });
}

// -------------------------------------------------- attendance days -----
function renderAttendanceDays(taps) {
  const box = $("attendance-days");
  if (!taps.length) { box.innerHTML = '<p class="muted">No data yet</p>'; return; }

  // Group by child + day
  const groups = {};
  taps.forEach(t => {
    const d = new Date(t.recorded_at);
    const day = d.toISOString().slice(0, 10);
    const key = `${t.card_uid}||${day}`;
    if (!groups[key]) {
      groups[key] = {
        name: t.cards?.child_name || t.card_uid,
        day, dayLabel: d.toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" }),
        first: d, last: d, count: 0,
      };
    }
    const g = groups[key];
    g.count++;
    if (d < g.first) g.first = d;
    if (d > g.last) g.last = d;
  });

  // Sort: most recent day first, then by name
  const rows = Object.values(groups).sort((a, b) => {
    if (b.day !== a.day) return b.day.localeCompare(a.day);
    return a.name.localeCompare(b.name);
  });

  // Show last 14 days max
  const shown = rows.slice(0, 40);
  box.innerHTML = "";
  shown.forEach(g => {
    const el = document.createElement("div");
    el.className = "att-day";
    const timeFmt = { hour: "2-digit", minute: "2-digit" };
    el.innerHTML = `
      <span class="aname">${g.name}</span>
      <span class="aday">${g.dayLabel}</span>
      <span class="atime">In ${g.first.toLocaleTimeString([], timeFmt)} – Out ${g.last.toLocaleTimeString([], timeFmt)}</span>
      <span class="acount">${g.count} tap${g.count !== 1 ? "s" : ""}</span>`;
    box.appendChild(el);
  });
}

boot();
