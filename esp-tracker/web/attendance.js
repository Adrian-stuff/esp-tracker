// Attendance dashboard — gate taps, daily in/out, and gate-scanner status.
//
// sb, $, secsAgo, ago and the sign-in/up wiring live in common.js, shared with
// tracker.js. This file only builds what is specific to school-gate staff:
// scanner health/rename and the tap history. Same RLS rules as the parent
// view apply — a signed-in account only ever sees devices it has been
// granted (device_access), so a school account still can't see another
// family's tracker just by knowing this page exists.

const HEALTH_STALE_S = 5 * 60;

requireAuth(async () => {
  await refresh();
  subscribe();
  setInterval(refresh, 30000);
});

// ---------------------------------------------------------------- data -----
async function refresh() {
  const { data: devices } = await sb.from("devices").select("*").eq("active", true);
  const scanners = (devices ?? []).filter((d) => d.kind === "scanner");

  const enriched = [];
  for (const s of scanners) {
    const { data: health } = await sb.from("device_health")
      .select("reported_at,battery_pct,signal_csq,queue_depth,uptime_s,reset_reason")
      .eq("device_id", s.id).order("reported_at", { ascending: false }).limit(1).maybeSingle();
    enriched.push({ ...s, health });
  }
  renderScanners(enriched);

  const [{ data: taps }, { data: cards }] = await Promise.all([
    sb.from("attendance")
      .select("card_uid,direction,recorded_at")
      .order("recorded_at", { ascending: false }).limit(200),
    sb.from("cards").select("card_uid,child_name"),
  ]);
  const cardMap = Object.fromEntries((cards ?? []).map((c) => [c.card_uid, c.child_name]));
  const allTaps = (taps ?? []).map((t) => ({
    ...t,
    child_name: cardMap[t.card_uid] || t.card_uid,
    cards: { child_name: cardMap[t.card_uid] || t.card_uid },
  }));
  renderAttendance(allTaps.slice(0, 8));
  renderAttendanceDays(allTaps);
}

// ------------------------------------------------------------- scanners ----
function renderScanners(list) {
  const box = $("scanners");
  if (!list.length) {
    box.innerHTML = '<p class="muted">No gate stations registered for this account yet. '
      + "Ask whoever provisioned the scanner to grant this account access.</p>";
    return;
  }
  box.innerHTML = "";
  list.forEach((s) => {
    const seenAge = s.last_seen_at ? secsAgo(s.last_seen_at) : null;
    const online = seenAge != null && seenAge < HEALTH_STALE_S;
    const h = s.health;
    const healthAge = h?.reported_at ? secsAgo(h.reported_at) : null;

    const el = document.createElement("div");
    el.className = "scanner-card";
    el.innerHTML = `
      <div class="sc-top">
        <span class="sname">${s.name || s.id}</span>
        <span class="sstatus ${online ? "on" : "off"}">${online ? "online" : (seenAge != null ? ago(seenAge) : "never seen")}</span>
      </div>
      <div class="sc-meta">
        <span>id <code>${s.id}</code></span>
        ${s.firmware ? `<span>fw ${s.firmware}</span>` : ""}
        ${s.battery_pct != null ? `<span>${s.battery_pct}% battery</span>` : ""}
        ${s.signal_csq != null ? `<span>CSQ ${s.signal_csq}</span>` : ""}
      </div>
      ${h ? `
      <div class="sc-health">
        <span class="sc-health-label">Last health report &middot; ${healthAge != null ? ago(healthAge) : "unknown"}</span>
        <div class="sc-meta">
          ${h.queue_depth != null ? `<span>${h.queue_depth} tap${h.queue_depth === 1 ? "" : "s"} queued offline</span>` : ""}
          ${h.uptime_s != null ? `<span>up ${Math.round(h.uptime_s / 3600)}h</span>` : ""}
          ${h.reset_reason ? `<span>last reset: ${h.reset_reason}</span>` : ""}
        </div>
      </div>` : '<p class="muted sc-health-none">No health report received yet.</p>'}
      <div class="sc-actions">
        <button class="rename-btn" data-rename="${s.id}" data-name="${(s.name || "").replace(/"/g, "&quot;")}">Rename</button>
      </div>`;
    box.appendChild(el);
  });

  box.querySelectorAll("[data-rename]").forEach((b) =>
    b.addEventListener("click", async () => {
      const current = b.dataset.name || "";
      const next = prompt("Name for this gate station:", current);
      if (next == null || !next.trim() || next.trim() === current) return;
      b.disabled = true; b.textContent = "Saving…";
      const { error } = await sb.rpc("rename_device", { d: b.dataset.rename, new_name: next.trim() });
      b.disabled = false; b.textContent = "Rename";
      if (error) { alert(error.message); return; }
      refresh();
    }));
}

// -------------------------------------------------------------- taps -----
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

function renderAttendanceDays(taps) {
  const box = $("attendance-days");
  if (!taps.length) { box.innerHTML = '<p class="muted">No data yet</p>'; return; }

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

  const rows = Object.values(groups).sort((a, b) => {
    if (b.day !== a.day) return b.day.localeCompare(a.day);
    return a.name.localeCompare(b.name);
  });

  const shown = rows.slice(0, 60);
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

// ------------------------------------------------------------- realtime ----
function subscribe() {
  sb.channel("attendance-live")
    .on("postgres_changes", { event: "INSERT", schema: "public", table: "attendance" },
        () => refresh())
    .on("postgres_changes", { event: "UPDATE", schema: "public", table: "devices" },
        () => refresh())
    .on("postgres_changes", { event: "INSERT", schema: "public", table: "device_health" },
        () => refresh())
    .subscribe((status) => {
      const live = status === "SUBSCRIBED";
      $("conn").textContent = live ? "live" : "reconnecting…";
      $("conn").className = "chip " + (live ? "live" : "down");
    });
}
