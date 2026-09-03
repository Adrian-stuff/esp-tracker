// /dev — presentation control tool. Mocks a tracker's location trail and
// SOS events by calling the dev-mock Edge Function directly (a shared
// secret, not Supabase Auth — this tool isn't tied to any dashboard
// account or any real device's bearer token, see dev-mock/index.ts).

const SUPABASE_URL = "https://nvdumsbxspevpvligzlw.supabase.co";
const FN_URL = `${SUPABASE_URL}/functions/v1/dev-mock`;
const $ = (id) => document.getElementById(id);

let secret = localStorage.getItem("dev_mock_secret") || "";

function log(msg) {
  const li = document.createElement("li");
  const t = new Date().toLocaleTimeString();
  li.textContent = `${t} — ${msg}`;
  // Cheap heuristic, not a formal status param: every call site already
  // writes human sentences with "FAILED" for failures and one of these verbs
  // for a real completed action, so a colored dot costs nothing extra to
  // wire up at each call site individually.
  if (/fail/i.test(msg)) li.className = "log-fail";
  else if (/sent|fired|resolved|queued|complete/i.test(msg)) li.className = "log-ok";
  $("log").prepend(li);
  while ($("log").children.length > 40) $("log").removeChild($("log").lastChild);
}

async function callMock(body) {
  const r = await fetch(FN_URL, {
    method: "POST",
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${secret}` },
    body: JSON.stringify(body),
  });
  const data = await r.json().catch(() => ({}));
  return { ok: r.ok, status: r.status, data };
}

// ------------------------------------------------------------- gate -----
async function tryEnter(candidateSecret) {
  const prev = secret;
  secret = candidateSecret;
  // dev-mock has no "ping" action — an unknown action still authenticates
  // first and only THEN complains about the action, so a 401 specifically
  // means "wrong secret", anything else means the secret checked out.
  const { status } = await callMock({ action: "__check__" });
  if (status === 401) { secret = prev; return false; }
  localStorage.setItem("dev_mock_secret", candidateSecret);
  return true;
}

$("gate-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  $("gate-error").textContent = "";
  const ok = await tryEnter($("secret").value.trim());
  if (ok) { $("gate").hidden = true; $("app").hidden = false; initApp(); }
  else $("gate-error").textContent = "Wrong secret.";
});

$("forget").addEventListener("click", () => {
  localStorage.removeItem("dev_mock_secret");
  location.reload();
});

// ----------------------------------------------------------- map/path ----
let map, waypoints = [], waypointMarkers = [], pathLine = null, trailMarker = null;
let trailTimer = null, lastSosEventId = null;

function initApp() {
  map = L.map("map").setView([14.5995, 120.9842], 13);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 19, attribution: "&copy; OpenStreetMap",
  }).addTo(map);

  map.on("click", (e) => {
    waypoints.push([e.latlng.lat, e.latlng.lng]);
    redrawPath();
    log(`waypoint ${waypoints.length} added at ${e.latlng.lat.toFixed(5)}, ${e.latlng.lng.toFixed(5)}`);
  });

  $("clear-path").addEventListener("click", () => {
    waypoints = [];
    redrawPath();
    log("waypoints cleared");
  });

  $("start-trail").addEventListener("click", startTrail);
  $("stop-trail").addEventListener("click", stopTrail);
  $("send-one").addEventListener("click", sendOnePoint);
  $("trigger-sos").addEventListener("click", triggerSos);
  $("resolve-sos").addEventListener("click", resolveSos);
  $("notify-parent").addEventListener("click", notifyParent);
}

function redrawPath() {
  waypointMarkers.forEach((m) => map.removeLayer(m));
  waypointMarkers = waypoints.map((p, i) =>
    L.marker(p, { title: `waypoint ${i + 1}` }).addTo(map));
  if (pathLine) map.removeLayer(pathLine);
  if (waypoints.length > 1) pathLine = L.polyline(waypoints, { color: "#0b6e68", dashArray: "6 4" }).addTo(map);
  $("waypoint-count").textContent = `${waypoints.length} waypoint${waypoints.length === 1 ? "" : "s"}`;
}

// ------------------------------------------------------------- trail -----
function interpolate(path, t) {
  // t in [0,1] along the whole path, by cumulative straight-line distance.
  if (path.length === 1) return path[0];
  const segLens = [];
  let total = 0;
  for (let i = 0; i < path.length - 1; i++) {
    const d = Math.hypot(path[i + 1][0] - path[i][0], path[i + 1][1] - path[i][1]);
    segLens.push(d);
    total += d;
  }
  let target = t * total;
  for (let i = 0; i < segLens.length; i++) {
    if (target <= segLens[i] || i === segLens.length - 1) {
      const f = segLens[i] ? target / segLens[i] : 0;
      const a = path[i], b = path[i + 1];
      return [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f];
    }
    target -= segLens[i];
  }
  return path[path.length - 1];
}

function startTrail() {
  if (waypoints.length < 1) { log("add at least one waypoint first"); return; }
  const durationMs = parseFloat($("duration").value) * 60 * 1000;
  const intervalMs = parseFloat($("interval").value) * 1000;
  const battStart = parseFloat($("batt-start").value);
  const battEnd = parseFloat($("batt-end").value);
  const deviceId = $("device-id").value.trim();
  if (!deviceId) { log("set a device id first"); return; }

  const steps = Math.max(1, Math.round(durationMs / intervalMs));
  let step = 0;

  $("start-trail").disabled = true;
  $("stop-trail").disabled = false;
  $("trail-status").textContent = `Running: 0/${steps} points sent`;
  $("trail-progress").hidden = false;
  $("trail-progress-bar").style.width = "0%";
  log(`trail started: ${waypoints.length} waypoint(s), ${steps} points over ${$("duration").value} min`);

  const tick = async () => {
    const t = steps === 1 ? 1 : step / (steps - 1);
    const [lat, lon] = interpolate(waypoints, t);
    const battery_pct = Math.round(battStart + (battEnd - battStart) * t);

    if (trailMarker) map.removeLayer(trailMarker);
    trailMarker = L.circleMarker([lat, lon], { radius: 8, color: "#0b6e68", fillOpacity: 0.8 }).addTo(map);

    const { ok, data } = await callMock({
      action: "location", device_id: deviceId, lat, lon,
      accuracy_m: 8 + Math.round(Math.random() * 6), source: "gnss", battery_pct,
    });
    step++;
    $("trail-status").textContent = `Running: ${step}/${steps} points sent`;
    $("trail-progress-bar").style.width = `${Math.round((step / steps) * 100)}%`;
    log(ok ? `point ${step}/${steps} sent (batt ${battery_pct}%)` : `point ${step} FAILED: ${JSON.stringify(data)}`);

    if (step >= steps) { stopTrail(); log("trail complete"); }
  };

  tick();
  trailTimer = setInterval(tick, intervalMs);
}

function stopTrail() {
  if (trailTimer) { clearInterval(trailTimer); trailTimer = null; }
  $("start-trail").disabled = false;
  $("stop-trail").disabled = true;
  setTimeout(() => { $("trail-progress").hidden = true; }, 600);
}

async function sendOnePoint() {
  const deviceId = $("device-id").value.trim();
  if (!deviceId) { log("set a device id first"); return; }
  let lat, lon;
  if (waypoints.length) [lat, lon] = waypoints[waypoints.length - 1];
  else if (trailMarker) { const ll = trailMarker.getLatLng(); lat = ll.lat; lon = ll.lng; }
  else { const c = map.getCenter(); lat = c.lat; lon = c.lng; }
  const battery_pct = parseFloat($("batt-start").value) || 90;

  if (trailMarker) map.removeLayer(trailMarker);
  trailMarker = L.circleMarker([lat, lon], { radius: 8, color: "#0b6e68", fillOpacity: 0.8 }).addTo(map);

  const { ok, data } = await callMock({
    action: "location", device_id: deviceId, lat, lon,
    accuracy_m: 8 + Math.round(Math.random() * 6), source: "gnss", battery_pct,
  });
  log(ok ? `single point sent (batt ${battery_pct}%)` : `single point FAILED: ${JSON.stringify(data)}`);
}

// --------------------------------------------------------------- sos -----
async function triggerSos() {
  const deviceId = $("device-id").value.trim();
  if (!deviceId) { log("set a device id first"); return; }
  let lat, lon;
  if (trailMarker) { const ll = trailMarker.getLatLng(); lat = ll.lat; lon = ll.lng; }
  else if (waypoints.length) [lat, lon] = waypoints[waypoints.length - 1];
  else { const c = map.getCenter(); lat = c.lat; lon = c.lng; }

  const { ok, data } = await callMock({ action: "sos", device_id: deviceId, lat, lon, accuracy_m: 10 });
  if (ok) {
    lastSosEventId = data.event_id;
    $("resolve-sos").disabled = false;
    $("sos-active-badge").hidden = false;
    log(`mock SOS fired (drill, no real escalation): ${data.event_id}`);
  } else {
    log(`SOS FAILED: ${JSON.stringify(data)}`);
  }
}

async function resolveSos() {
  if (!lastSosEventId) return;
  const { ok } = await callMock({ action: "resolve", event_id: lastSosEventId });
  log(ok ? `resolved ${lastSosEventId}` : "resolve FAILED");
  if (ok) { $("resolve-sos").disabled = true; $("sos-active-badge").hidden = true; lastSosEventId = null; }
}

// ---------------------------------------------------- notify-parent -----
async function notifyParent() {
  const deviceId = $("device-id").value.trim();
  const to = $("notify-phone").value.trim();
  if (!deviceId) { log("set a device id first"); return; }
  if (!to) { log("type a phone number first — never auto-filled, on purpose"); return; }

  let lat, lon;
  if (trailMarker) { const ll = trailMarker.getLatLng(); lat = ll.lat; lon = ll.lng; }
  else if (waypoints.length) [lat, lon] = waypoints[waypoints.length - 1];
  else { const c = map.getCenter(); lat = c.lat; lon = c.lng; }

  const { ok, data } = await callMock({ action: "notify-parent", device_id: deviceId, to, lat, lon });
  if (ok) {
    log(`queued for relay by ${deviceId}'s own SIM800L (outbox #${data.outbox_id}): "${data.body}". ` +
        `Only actually sends if that device is online with WiFi configured — up to ~20s if so.`);
  } else {
    log(`notify-parent FAILED: ${JSON.stringify(data)}`);
  }
}

// ------------------------------------------------------------- boot -----
if (secret) {
  tryEnter(secret).then((ok) => {
    if (ok) { $("gate").hidden = true; $("app").hidden = false; initApp(); }
    else { localStorage.removeItem("dev_mock_secret"); }
  });
}
