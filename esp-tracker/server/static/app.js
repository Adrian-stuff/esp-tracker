// Parent dashboard.
//
// The rule this file exists to enforce: a position is never drawn without its
// source, its accuracy and its age. A 12 m GNSS fix and a 2 km cell estimate
// are both "a pin" if you let them be, and that is the most dangerous bug
// available in this system.

const STALE_S = 15 * 60;

// Encodes reliability in the mark itself, not in a legend nobody reads.
// Mid-tone and saturated rather than the muted UI palette, because these have
// to read on both basemaps. `firm` marks a fix precise enough for a SOLID
// accuracy ring; the rest get a dashed one, since a dashed boundary reads as
// "somewhere in here" and a solid one reads as "here".
const SOURCE = {
  gnss:       { label: "GPS",        color: "#12b886", firm: true  },
  wifi:       { label: "Wi-Fi",      color: "#3b8fd6", firm: true  },
  ble_anchor: { label: "beacon",     color: "#9b7cd4", firm: true  },
  cell:       { label: "cell tower", color: "#d9a441", firm: false },
  manual:     { label: "manual",     color: "#8a97a6", firm: false },
};

const PLACE_GLYPH = { home: "\u{1F3E0}", school: "\u{1F3EB}", other: "\u{1F4CD}" };

const map = L.map("map", { zoomControl: true }).setView([14.5995, 120.9842], 12);
// Plain OSM tiles, and the dark basemap comes from a CSS filter on the tile
// pane rather than a second provider.
//
// CARTO and Stadia both want an API key now, and a watermarked map is worse
// than a plain one. This keeps a single tile source whose usage policy we have
// already accepted, and the filter only touches .leaflet-tile-pane, so markers
// and overlays keep their true colours.
L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
  maxZoom: 19,
  attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
}).addTo(map);

const layers = { kid: null, ring: null, trail: [], places: [] };

const markers = {};   // device_id -> { dot, ring }
let fitted = false;
let fittedTrail = false;
let activeSos = null;

const $ = (id) => document.getElementById(id);

function ago(seconds) {
  if (seconds == null) return "never";
  if (seconds < 60) return `${seconds | 0}s ago`;
  if (seconds < 3600) return `${(seconds / 60) | 0} min ago`;
  if (seconds < 86400) return `${(seconds / 3600) | 0} hr ago`;
  return `${(seconds / 86400) | 0} days ago`;
}

function kidIcon(colour, initial, stale) {
  return L.divIcon({
    className: "", iconSize: [0, 0],
    html: `<div class="kid ${stale ? "stale" : ""}" style="--c:${colour}">
             <span class="halo"></span><span class="ring"></span>
             <span class="dot">${initial}</span></div>`,
  });
}

function drawDevice(d) {
  const loc = d.location;
  if (!loc) return;
  const meta = SOURCE[loc.source] || SOURCE.manual;
  const stale = loc.stale;
  const initial = (d.child_name || d.name || "?").trim()[0].toUpperCase();

  // Sized from the REAL accuracy, so a cell fix looks like the kilometre-wide
  // guess it is instead of a confident dot.
  if (!layers.ring) layers.ring = L.circle([loc.lat, loc.lon], { radius: loc.accuracy_m }).addTo(map);
  layers.ring.setLatLng([loc.lat, loc.lon]).setRadius(loc.accuracy_m).setStyle({
    color: meta.color, weight: 1.5,
    dashArray: meta.firm ? null : "5 5",
    fillColor: meta.color, fillOpacity: stale ? 0.04 : 0.10,
    opacity: stale ? 0.30 : 0.65,
  });

  if (!layers.kid) layers.kid = L.marker([loc.lat, loc.lon], { zIndexOffset: 900 }).addTo(map);
  layers.kid.setLatLng([loc.lat, loc.lon]);
  layers.kid.setIcon(kidIcon(stale ? "#8a97a6" : meta.color, initial, stale));
  layers.kid.bindPopup(
    `<b>${d.child_name || d.name}</b><br>${loc.place ? `<b>At ${loc.place}</b><br>` : ""}` +
    `<span class="src">${meta.label} &middot; &plusmn;${Math.round(loc.accuracy_m)} m ` +
    `&middot; ${ago(loc.age_s)}</span>`);

  updateFreshness(loc, meta);
  if (!fitted && !fittedTrail) { map.setView([loc.lat, loc.lon], 16); fitted = true; }
}

// Age belongs OVER the map. It is the one fact deciding whether anything else
// on screen can be trusted, so it should never need a look at the sidebar.
function updateFreshness(loc, meta) {
  const box = $("freshness");
  box.hidden = false;
  box.className = loc.age_s > 3600 ? "dead" : loc.stale ? "stale" : "";
  $("fresh-age").textContent =
    loc.stale ? `Last seen ${ago(loc.age_s)}` : `Updated ${ago(loc.age_s)}`;
  $("fresh-detail").textContent =
    `${meta.label} \u00b7 \u00b1${Math.round(loc.accuracy_m)} m` +
    (loc.place ? ` \u00b7 ${loc.place}` : "");
}

// Named places as zones. Once a parent has said "these networks are school",
// the map should show school, not an anonymous circle.
function drawPlaces(places) {
  layers.places.forEach((l) => map.removeLayer(l));
  layers.places = [];
  (places || []).forEach((p) => {
    if (p.lat == null || p.lon == null) return;
    layers.places.push(L.circle([p.lat, p.lon], {
      radius: Math.max(p.radius_m || 40, 45),
      color: "#8a97a6", weight: 1.5, dashArray: "3 6",
      fillColor: "#8a97a6", fillOpacity: 0.07, opacity: 0.5, interactive: false,
    }).addTo(map));
    layers.places.push(L.marker([p.lat, p.lon], {
      interactive: false, zIndexOffset: 400,
      icon: L.divIcon({ className: "", iconSize: [0, 0],
        html: `<div class="place-label"><span class="g">${PLACE_GLYPH[p.kind] || PLACE_GLYPH.other}</span>${p.name}</div>` }),
    }).addTo(map));
  });
}

// The day's route, coloured by the source behind each leg, so the cell-only
// stretch visibly degrades instead of blending in with the GPS.
let trailPoints = [];

async function drawTrail(deviceId) {
  const r = await fetch(`/api/devices/${deviceId}/history?frm=1&limit=1000`,
                        { credentials: "same-origin" });
  if (!r.ok) return;
  trailPoints = await r.json();
  if (!trailPoints.length) return;

  if (!fittedTrail) {
    fittedTrail = true;
    // Open on the whole day rather than a fixed zoom: a parent glancing at this
    // wants "where has she been", and the current position is inside those
    // bounds anyway, so the context costs nothing.
    map.fitBounds(L.latLngBounds(trailPoints.map((p) => [p.lat, p.lon])),
                  { padding: [50, 50], maxZoom: 17 });
  }
  initScrubber();
  renderTrail();
}

// Scrubbing filters the CACHED points. Refetching per drag would put a 2G-era
// round trip behind a control people expect to feel instant.
function renderTrail() {
  layers.trail.forEach((l) => map.removeLayer(l));
  layers.trail = [];
  const [from, to] = scrubWindow();
  const pts = trailPoints.filter((p) => p.recorded_at >= from && p.recorded_at <= to);

  $("scrub-count").textContent = pts.length
    ? `${pts.length} of ${trailPoints.length} fixes`
    : "no fixes in this window";

  for (let i = 1; i < pts.length; i++) {
    const a = pts[i - 1], b = pts[i];
    const meta = SOURCE[b.source] || SOURCE.manual;
    // A long silence is a gap in knowledge, not a straight walk. Dash it.
    const gap = b.recorded_at - a.recorded_at > 20 * 60;
    layers.trail.push(L.polyline([[a.lat, a.lon], [b.lat, b.lon]], {
      color: meta.color, weight: gap ? 2 : 3.5,
      opacity: gap ? 0.30 : 0.55, dashArray: gap ? "4 8" : null,
      interactive: false,
    }).addTo(map));
  }
}

// ---------------------------------------------------------------- scrubber --
// The range is minutes-of-LOCAL-day, because that is the unit a parent thinks
// in ("was she still at school at three?"), not epoch seconds.
let dayStart = 0;

function scrubWindow() {
  const a = +$("scrub-from").value, b = +$("scrub-to").value;
  const lo = Math.min(a, b), hi = Math.max(a, b);
  return [dayStart + lo * 60, dayStart + hi * 60 + 59];
}

function hhmm(mins) {
  return `${String(Math.floor(mins / 60)).padStart(2, "0")}:${String(mins % 60).padStart(2, "0")}`;
}

function updateScrubLabel() {
  const a = +$("scrub-from").value, b = +$("scrub-to").value;
  $("scrub-range").textContent = `${hhmm(Math.min(a, b))} \u2013 ${hhmm(Math.max(a, b))}`;
}

let scrubberReady = false;
function initScrubber() {
  if (scrubberReady || !trailPoints.length) return;
  scrubberReady = true;

  const first = new Date(trailPoints[0].recorded_at * 1000);
  first.setHours(0, 0, 0, 0);
  dayStart = Math.floor(first.getTime() / 1000);

  $("scrub").hidden = false;
  const from = $("scrub-from"), to = $("scrub-to");

  // Start on the span the data actually covers, not 00:00-23:59 — most of a
  // 24 hour range is empty and the handles would sit in dead space.
  const mins = (t) => Math.floor((t - dayStart) / 60);
  from.value = Math.max(0, mins(trailPoints[0].recorded_at) - 10);
  to.value = Math.min(1439, mins(trailPoints[trailPoints.length - 1].recorded_at) + 10);

  [from, to].forEach((el) => el.addEventListener("input", () => {
    document.querySelectorAll(".scrub-presets button").forEach((b) => b.classList.remove("on"));
    updateScrubLabel();
    renderTrail();
  }));

  document.querySelectorAll(".scrub-presets button").forEach((btn) =>
    btn.addEventListener("click", () => {
      document.querySelectorAll(".scrub-presets button").forEach((b) => b.classList.remove("on"));
      btn.classList.add("on");
      const nowMin = Math.floor((Date.now() / 1000 - dayStart) / 60);
      const p = btn.dataset.preset;
      if (p === "all")       { from.value = 0;    to.value = 1439; }
      else if (p === "am")   { from.value = 300;  to.value = 720; }
      else if (p === "pm")   { from.value = 720;  to.value = 1140; }
      else if (p === "last") { from.value = Math.max(0, nowMin - 60); to.value = Math.min(1439, nowMin); }
      updateScrubLabel();
      renderTrail();
    }));

  updateScrubLabel();
}

function renderDevices(list) {
  const box = $("devices");
  box.innerHTML = "";
  list.filter((d) => d.kind === "tracker").forEach((d) => {
    const loc = d.location;
    const el = document.createElement("div");
    el.className = "dev" + (loc && loc.stale ? " stale" : "");

    const src = loc ? (SOURCE[loc.source] || SOURCE.manual) : null;
    const battery = d.battery_pct == null ? "—" : `${d.battery_pct}%`;

    el.innerHTML = `
      <div class="top">
        <span class="name">${d.child_name || d.name}</span>
        <span class="age">${loc ? ago(loc.age_s) : "no fix yet"}</span>
      </div>
      ${loc && loc.place ? `<div class="place">At ${loc.place}</div>` : ""}
      <div class="meta">
        <span><b>${battery}</b> battery</span>
        ${loc ? `<span>${src.label} &plusmn;${Math.round(loc.accuracy_m)}m</span>` : ""}
        ${d.signal_csq != null ? `<span>CSQ ${d.signal_csq}</span>` : ""}
      </div>
      ${d.balance_pesos != null && d.balance_pesos < 20
        ? `<div class="warn">Load is low (₱${d.balance_pesos}) — top up or the tracker goes silent.</div>` : ""}
      ${d.battery_pct != null && d.battery_pct <= 20
        ? `<div class="warn">Battery ${d.battery_pct}% — charge tonight.</div>` : ""}
      <button data-locate="${d.id}">Locate now</button>`;

    box.appendChild(el);
    if (loc) drawDevice(d);
  });

  box.querySelectorAll("[data-locate]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      btn.disabled = true;
      btn.textContent = "Waking the tracker…";
      try {
        // Server texts the device; the modem wakes on RI even from sleep.
        await fetch(`/api/devices/${btn.dataset.locate}/locate`, { method: "POST" });
        btn.textContent = "Asked — expect a fix in ~20s";
      } catch {
        btn.textContent = "Couldn't reach the tracker";
      }
      setTimeout(() => { btn.disabled = false; btn.textContent = "Locate now"; }, 25000);
    });
  });
}

function showSos(payload) {
  activeSos = payload.event_id;
  $("sos-title").textContent = "SOS — button pressed";
  $("sos-detail").textContent = payload.text || "Position not yet known.";
  $("sos-banner").hidden = false;
  try { new Audio("/static/alarm.mp3").play().catch(() => {}); } catch {}
  if (payload.location) {
    map.setView([payload.location.lat, payload.location.lon], 17);
  }
}

$("sos-ack").addEventListener("click", async () => {
  if (!activeSos) return;
  // Halts the escalation ladder server-side and records who answered.
  await fetch(`/api/sos/${activeSos}/ack`, {
    method: "POST", headers: { "content-type": "application/json" }, body: "{}" });
  $("sos-banner").hidden = true;
  activeSos = null;
});

// A tap at the gate is news. It should announce itself rather than quietly
// appearing in a list the parent may not be looking at.
function toast(glyph, title, detail, kind) {
  const el = document.createElement("div");
  el.className = `toast ${kind || ""}`;
  el.innerHTML = `<span class="g">${glyph}</span><div class="t"><strong>${title}</strong><span>${detail}</span></div>`;
  $("toasts").appendChild(el);
  setTimeout(() => el.remove(), 7000);
}

let lastTapKey = null;

function renderAttendance(rows) {
  const ul = $("attendance");
  if (!rows.length) { ul.innerHTML = '<li class="muted">No taps yet</li>'; return; }
  ul.innerHTML = "";
  rows.slice(0, 8).forEach((r, i) => {
    const li = document.createElement("li");
    const t = new Date(r.recorded_at * 1000);
    const when = t.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    const who = r.child_name || r.card_uid;
    li.innerHTML = `<span>${who} tapped ${r.direction}</span><time>${when}</time>`;

    // Newest row only, and only when it is genuinely new to this session.
    const key = `${r.card_uid}-${r.recorded_at}`;
    if (i === 0 && lastTapKey !== null && key !== lastTapKey) {
      li.classList.add("fresh");
      toast(r.direction === "in" ? "\u{1F3EB}" : "\u{1F44B}",
            `${who} tapped ${r.direction}`,
            `${when} \u00b7 at the gate`,
            r.direction === "out" ? "out" : "");
    }
    if (i === 0) lastTapKey = key;
    ul.appendChild(li);
  });
  if (!rows.length) lastTapKey = null;
}

// ---------------------------------------------------------------- places ---
// A parent can see what the tracker sees and name it. This is what turns a
// 30 m circle into "At school since 08:41" — and a named place is resolved
// on-device, so those networks are never sent to the geolocation provider.

let primaryDevice = null;

function signalWord(rssi) {
  if (rssi == null) return "";
  if (rssi >= -60) return "strong";
  if (rssi >= -75) return "ok";
  return "weak";
}

async function loadPlaces(deviceId) {
  if (!deviceId) return;
  const d = await fetch(`/api/devices/${deviceId}/wifi`).then((r) => r.json());

  drawPlaces(d.places);

  // Saved places
  const box = $("places");
  box.innerHTML = d.places.length
    ? ""
    : '<p class="muted">No places yet. Name one below so the map can say where your child is, not just where.</p>';
  d.places.forEach((p) => {
    const row = document.createElement("div");
    row.className = "place-row";
    row.innerHTML = `<span class="pname">${p.name}</span>` +
      `<span class="pmeta">${p.ap_count} network${p.ap_count === 1 ? "" : "s"}</span>` +
      `<button data-del="${p.id}">Remove</button>`;
    box.appendChild(row);
  });
  box.querySelectorAll("[data-del]").forEach((b) =>
    b.addEventListener("click", async () => {
      await fetch(`/api/places/${b.dataset.del}`, { method: "DELETE" });
      loadPlaces(deviceId);
    }));

  // Two groups, because they answer different questions.
  //
  // "Visible now" is what the tracker can see FROM WHERE THE CHILD IS. That is
  // the list you pick from when you are standing at the school gate naming the
  // place. "Seen before" is history — useful for recognising a network, useless
  // for confirming you are in the right building right now.
  //
  // The old flat list mixed the two, so a network last seen on Tuesday looked
  // exactly like one three metres away.
  const list = $("ap-list");
  const liveRssi = new Map(d.current.map((a) => [a.h, a.rssi]));
  const byHash = new Map(d.frequent.map((a) => [a.h, a]));
  d.current.forEach((a) => { if (!byHash.has(a.h)) byHash.set(a.h, { ...a, seen: 1 }); });

  const live = [...byHash.values()].filter((a) => liveRssi.has(a.h))
                 .sort((x, y) => (liveRssi.get(y.h) ?? -100) - (liveRssi.get(x.h) ?? -100));
  const past = [...byHash.values()].filter((a) => !liveRssi.has(a.h))
                 .sort((x, y) => (y.seen || 0) - (x.seen || 0));

  if (!live.length && !past.length) {
    list.innerHTML = '<p class="muted">No Wi-Fi scans yet. They arrive with the next report.</p>';
    return;
  }

  const row = (a, isLive) => {
    const rssi = isLive ? liveRssi.get(a.h) : a.rssi;
    const name = a.ssid ? a.ssid : "(hidden network)";
    const meta = isLive
      ? `<span class="sig">${signalWord(rssi)}</span>`
      : `<span class="when">${a.last_seen_s != null ? ago(a.last_seen_s) : "earlier"}</span>`;
    return `<label class="ap ${isLive ? "live" : "past"}">
        <input type="checkbox" value="${a.h}" ${a.place ? "disabled" : ""}>
        ${bars(rssi, isLive)}
        <span class="ssid ${a.ssid ? "" : "unnamed"}">${name}</span>
        <span class="apmeta">
          ${a.place ? `<span class="taken">${a.place}</span>` : ""}
          ${a.seen > 1 ? `<span class="count">${a.seen}&times;</span>` : ""}
          ${meta}
        </span>
      </label>`;
  };

  const age = d.last_scan_age_s;
  list.innerHTML =
    (live.length
      ? `<div class="ap-head live"><span class="dot"></span>Visible now
           <em>${age != null ? `scanned ${ago(age)}` : ""}</em></div>` +
        live.map((a) => row(a, true)).join("")
      : `<div class="ap-head live"><span class="dot off"></span>Nothing visible in the last scan</div>`) +
    (past.length
      ? `<div class="ap-head">Seen before <em>${past.length}</em></div>` +
        past.map((a) => row(a, false)).join("")
      : "");
}

// Four bars from RSSI. Signal strength is the thing that tells a parent
// "you are inside the building", so it gets a shape rather than a word.
function bars(rssi, isLive) {
  const n = rssi == null ? 0
          : rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
  let out = `<span class="sigbars ${isLive ? "" : "dim"}">`;
  for (let i = 1; i <= 4; i++) out += `<i class="${i <= n ? "on" : ""}"></i>`;
  return out + "</span>";
}

$("place-save").addEventListener("click", async () => {
  const btn = $("place-save");
  const name = $("place-name").value.trim();
  const picked = [...document.querySelectorAll("#ap-list input:checked")].map((i) => i.value);
  if (!name || !picked.length) {
    btn.textContent = !name ? "Name it first" : "Tick some networks";
    setTimeout(() => (btn.textContent = "Save place"), 2000);
    return;
  }
  btn.disabled = true;
  await fetch(`/api/devices/${primaryDevice}/places`, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ name, kind: $("place-kind").value, bssid_hashes: picked }),
  });
  $("place-name").value = "";
  btn.disabled = false;
  btn.textContent = "Saved";
  setTimeout(() => (btn.textContent = "Save place"), 1500);
  loadPlaces(primaryDevice);
});

async function refresh() {
  try {
    const rs = await Promise.all([
      fetch("/api/devices", { credentials: "same-origin" }),
      fetch("/api/attendance?limit=8", { credentials: "same-origin" }),
      fetch("/api/sos", { credentials: "same-origin" }),
    ]);

    // A session that expires while the dashboard is open must not leave a
    // parent staring at a frozen map. Before this, the poller just 401'd every
    // 30 s forever and the last known position sat there looking current —
    // exactly the silent-staleness failure this whole design exists to avoid.
    if (rs.some((r) => r.status === 401)) {
      sessionLost();
      return;
    }
    const [devices, taps, sos] = await Promise.all(rs.map((r) => r.json()));
    renderDevices(devices);
    renderAttendance(taps);
    const tracker = devices.find((d) => d.kind === "tracker");
    if (tracker && tracker.id !== primaryDevice) {
      primaryDevice = tracker.id;
      loadPlaces(primaryDevice);
      drawTrail(primaryDevice);
    }
    const open = sos.find((s) => s.status === "open");
    if (open && !activeSos) {
      showSos({ event_id: open.event_id,
                text: open.lat ? `±${Math.round(open.accuracy_m)} m via ${open.source}` : "" ,
                location: open.lat ? { lat: open.lat, lon: open.lon } : null });
    }
  } catch (e) {
    $("conn").textContent = "offline"; $("conn").className = "chip down";
  }
}

let ws = null;

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws/live`);
  ws.onopen  = () => { $("conn").textContent = "live"; $("conn").className = "chip live"; };
  ws.onclose = () => {
    if (!started) return;          // deliberate close on sign-out
    $("conn").textContent = "reconnecting…"; $("conn").className = "chip down";
    setTimeout(connect, 3000);
  };
  ws.onmessage = (e) => {
    const { kind, payload } = JSON.parse(e.data);
    if (kind === "sos") showSos(payload);
    else if (kind === "sos_ack") { $("sos-banner").hidden = true; activeSos = null; }
    else refresh();
  };
}

// ---------------------------------------------------------------- auth -----
let started = false;
let pollTimer = null;

function sessionLost() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  started = false;
  if (ws) { try { ws.close(); } catch {} ws = null; }
  $("conn").textContent = "signed out"; $("conn").className = "chip down";
  $("signin").hidden = false;
  $("signin-error").textContent = "Your session expired. Please sign in again.";
}

async function boot() {
  // A 401 here just means "not signed in yet" — show the form rather than an
  // empty map that looks like a broken tracker. Returns whether we are in.
  let me;
  try {
    // /api/auth/me always answers 200, so a normal signed-out load produces no
    // console error and a real 401 elsewhere stays meaningful.
    me = await (await fetch("/api/auth/me", { credentials: "same-origin" })).json();
  } catch {
    $("conn").textContent = "offline"; $("conn").className = "chip down";
    return false;
  }
  if (!me.authenticated) { $("signin").hidden = false; return false; }

  $("signin").hidden = true;
  if (!started) {
    started = true;
    connect();
    pollTimer = setInterval(refresh, 30000);
  }
  await refresh();
  if (map) map.invalidateSize();   // the map was laid out under the overlay
  return true;
}

$("signin-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const btn = e.submitter || $("signin-form").querySelector("button");
  const err = $("signin-error");
  err.textContent = "";
  btn.disabled = true;
  const label = btn.textContent;
  btn.textContent = "Signing in\u2026";

  try {
    const r = await fetch("/api/auth/login", {
      method: "POST",
      headers: { "content-type": "application/json" },
      credentials: "same-origin",
      body: JSON.stringify({ email: $("email").value, password: $("password").value }),
    });

    if (!r.ok) {
      let detail = "Sign in failed";
      try { detail = (await r.json()).detail || detail; } catch {}
      err.textContent = detail;
      return;
    }

    // No location.reload(). A reload re-runs boot(), and any hiccup in between
    // just shows the form again with no explanation — which is indistinguishable
    // from the button doing nothing. Going straight to boot() means the click
    // always visibly resolves, one way or the other.
    const ok = await boot();
    if (!ok) {
      // Signed in, but the session did not come back on the next request. In
      // practice this is COOKIE_SECURE=1 while being served over plain http,
      // so the browser accepts the response and drops the cookie.
      err.textContent =
        "Signed in, but the session did not stick. If this server is on http, " +
        "start it with COOKIE_SECURE=0.";
    }
  } catch (e) {
    err.textContent = "Could not reach the server.";
  } finally {
    btn.disabled = false;
    btn.textContent = label;
  }
});

$("signout").addEventListener("click", async () => {
  await fetch("/api/auth/logout", { method: "POST", credentials: "same-origin" });
  location.reload();
});

// If sign-in ever "does nothing" again, check this line in the console first:
// a missing or old stamp means the browser is running a cached script.
window.TRACKER_BUILD = "scrubber-taps";
console.info("tracker dashboard build:", window.TRACKER_BUILD);

// Polling backs up the realtime feed; the device reports every 2 min at best,
// so anything faster only burns the parent's data.
boot();
