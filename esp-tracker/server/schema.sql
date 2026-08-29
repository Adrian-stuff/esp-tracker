-- Child tracker — schema (SQLite for the prototype; Postgres-ready).
--
-- Two rules encoded structurally, not by convention:
--   1. recorded_at and received_at are SEPARATE columns and both are stored.
--      On a 2G link they can differ by hours, and collapsing them silently
--      turns a queued event into a lie about where the child is now.
--   2. Every position carries its SOURCE and an honest accuracy_m. Conflating a
--      12 m GNSS fix with a 2 km cell estimate is the most dangerous bug
--      available in this system.

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS devices (
    id              TEXT PRIMARY KEY,          -- "tracker-01", "scanner-gate-01"
    kind            TEXT NOT NULL CHECK (kind IN ('tracker','scanner')),
    name            TEXT NOT NULL,
    child_name      TEXT,
    token_hash      TEXT NOT NULL,             -- per-device bearer, hashed
    msisdn          TEXT,                      -- for the SMS downlink ("locate now")
    firmware        TEXT,
    battery_pct     INTEGER,
    signal_csq      INTEGER,
    balance_pesos   INTEGER,                   -- prepaid load; silent killer if unwatched
    last_seen_at    INTEGER,
    active          INTEGER NOT NULL DEFAULT 1,
    created_at      INTEGER NOT NULL
);

-- Registered BSSID sets / BLE anchors. When one matches, NO geolocation API is
-- called at all: faster, free, and the provider never learns the child's routine.
CREATE TABLE IF NOT EXISTS places (
    id              INTEGER PRIMARY KEY,
    device_id       TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    name            TEXT NOT NULL,             -- "school", "home"
    kind            TEXT NOT NULL DEFAULT 'other' CHECK (kind IN ('home','school','other')),
    bssid_set       TEXT,                      -- JSON array of hashed BSSIDs
    ble_anchor_id   TEXT,
    lat             REAL, lon REAL, radius_m REAL
);

-- Recent Wi-Fi scans, so a parent can look at what their child's tracker can
-- actually see and say "those networks are school".
--
-- BSSIDs are stored HASHED. An AP-level history is a finer-grained record of
-- where a child has been than the coordinates are, so a database dump must not
-- hand one over. The SSID is kept in clear because it is the only part a parent
-- can recognise, and matching runs on the hashes.
CREATE TABLE IF NOT EXISTS wifi_scans (
    id              INTEGER PRIMARY KEY,
    device_id       TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    recorded_at     INTEGER NOT NULL,
    aps             TEXT NOT NULL,             -- JSON [{h, ssid, rssi}]
    place_id        INTEGER REFERENCES places(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS ix_wifi_device_time ON wifi_scans(device_id, recorded_at DESC);

CREATE TABLE IF NOT EXISTS locations (
    id              INTEGER PRIMARY KEY,
    event_id        TEXT UNIQUE,               -- device-generated; makes retries idempotent
    device_id       TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    lat             REAL NOT NULL,
    lon             REAL NOT NULL,
    accuracy_m      REAL NOT NULL,
    source          TEXT NOT NULL CHECK (source IN ('gnss','wifi','ble_anchor','cell','manual')),
    place_id        INTEGER REFERENCES places(id) ON DELETE SET NULL,
    recorded_at     INTEGER NOT NULL,          -- when it HAPPENED, on the device
    received_at     INTEGER NOT NULL,          -- when the server got it
    battery_pct     INTEGER,
    speed_mps       REAL,
    heading         REAL
);
CREATE INDEX IF NOT EXISTS ix_loc_device_time ON locations(device_id, recorded_at DESC);

CREATE TABLE IF NOT EXISTS sos_events (
    id              INTEGER PRIMARY KEY,
    event_id        TEXT UNIQUE NOT NULL,
    device_id       TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    triggered_at    INTEGER NOT NULL,
    received_at     INTEGER NOT NULL,
    latency_ms      INTEGER,
    first_location_id INTEGER REFERENCES locations(id),
    best_location_id  INTEGER REFERENCES locations(id),
    -- The device sends its own SMS straight to the parent. When it reports that
    -- it did, the server skips the t+0 SMS and starts the ladder one rung up,
    -- so one press never becomes two texts.
    device_sms_sent INTEGER NOT NULL DEFAULT 0,
    status          TEXT NOT NULL DEFAULT 'open'
                    CHECK (status IN ('open','acknowledged','resolved','false_alarm')),
    acknowledged_by TEXT, acknowledged_at INTEGER,
    resolved_at     INTEGER,
    is_drill        INTEGER NOT NULL DEFAULT 0,
    notes           TEXT
);

-- Audit trail: proves what actually reached whom. Without it you are guessing.
CREATE TABLE IF NOT EXISTS alerts (
    id              INTEGER PRIMARY KEY,
    sos_event_id    INTEGER NOT NULL REFERENCES sos_events(id) ON DELETE CASCADE,
    channel         TEXT NOT NULL CHECK (channel IN ('push','sms','voice','email','websocket')),
    recipient       TEXT NOT NULL,
    sent_at         INTEGER NOT NULL,
    provider_msg_id TEXT,
    delivery_status TEXT NOT NULL DEFAULT 'sent',
    error           TEXT
);

-- Outbound SMS the DEVICES can send on the server's behalf.
--
-- Both modems are already paid for and, on a matching PH network, a device-sent
-- text is effectively free where a provider charges per message. So routine
-- notifications are offered to the devices first and only fall through to the
-- paid provider if nobody delivers them.
--
-- SOS NEVER ENTERS THIS TABLE. An emergency must not wait for a device to poll,
-- claim and confirm; the escalation ladder keeps using the provider directly.
--
-- child_device_id encodes the privacy rule: a scanner may relay anything, but a
-- TRACKER may only relay messages concerning its own child, whose parent number
-- it already holds for SOS. Without that, a general relay would push other
-- families' numbers onto a child's device.
CREATE TABLE IF NOT EXISTS outbox (
    id              INTEGER PRIMARY KEY,
    to_number       TEXT NOT NULL,
    body            TEXT NOT NULL,
    child_device_id TEXT REFERENCES devices(id) ON DELETE SET NULL,
    status          TEXT NOT NULL DEFAULT 'pending'
                    CHECK (status IN ('pending','claimed','sent','provider_sent','failed','expired')),
    claimed_by      TEXT,
    lease_until     INTEGER,          -- stops two devices sending the same message
    fallback_after  INTEGER NOT NULL, -- after this the server stops waiting and pays
    attempts        INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    sent_at         INTEGER,
    ref             TEXT,             -- originating event, for audit
    error           TEXT
);
CREATE INDEX IF NOT EXISTS ix_outbox_pending ON outbox(status, fallback_after);

-- Taps from the RFID scanner at the school gate.
CREATE TABLE IF NOT EXISTS attendance (
    id              INTEGER PRIMARY KEY,
    event_id        TEXT UNIQUE,
    scanner_id      TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    card_uid        TEXT NOT NULL,
    child_device_id TEXT REFERENCES devices(id) ON DELETE SET NULL,
    direction       TEXT NOT NULL DEFAULT 'in' CHECK (direction IN ('in','out')),
    device_sms_sent INTEGER NOT NULL DEFAULT 0,
    recorded_at     INTEGER NOT NULL,
    received_at     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_att_time ON attendance(recorded_at DESC);

-- Card UID -> child. Lives ONLY on the server: a stolen scanner must not reveal
-- who attends the school.
CREATE TABLE IF NOT EXISTS cards (
    card_uid        TEXT PRIMARY KEY,
    device_id       TEXT REFERENCES devices(id) ON DELETE SET NULL,
    child_name      TEXT NOT NULL,
    active          INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS geofences (
    id              INTEGER PRIMARY KEY,
    device_id       TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    name            TEXT NOT NULL,
    lat REAL NOT NULL, lon REAL NOT NULL, radius_m REAL NOT NULL,
    alert_on        TEXT NOT NULL DEFAULT 'both' CHECK (alert_on IN ('enter','exit','both'))
);

CREATE TABLE IF NOT EXISTS device_health (
    id              INTEGER PRIMARY KEY,
    device_id       TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    reported_at     INTEGER NOT NULL,
    battery_pct     INTEGER, signal_csq INTEGER, queue_depth INTEGER,
    uptime_s        INTEGER, reset_reason TEXT
);

CREATE TABLE IF NOT EXISTS users (
    id              INTEGER PRIMARY KEY,
    email           TEXT UNIQUE NOT NULL,
    password_hash   TEXT NOT NULL,             -- argon2id
    totp_secret     TEXT,
    role            TEXT NOT NULL DEFAULT 'parent' CHECK (role IN ('parent','viewer','admin')),
    phone           TEXT,
    escalation_order INTEGER NOT NULL DEFAULT 1,   -- 1 = primary, 2 = secondary
    created_at      INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS sessions (
    token_hash      TEXT PRIMARY KEY,
    user_id         INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at      INTEGER NOT NULL,
    revoked_at      INTEGER
);

-- Explicit grants. No implicit "sees everything" — enforced in every query.
CREATE TABLE IF NOT EXISTS device_access (
    user_id         INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id       TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    PRIMARY KEY (user_id, device_id)
);

-- Who looked at which child's location, and when.
CREATE TABLE IF NOT EXISTS access_log (
    id              INTEGER PRIMARY KEY,
    user_id         INTEGER REFERENCES users(id) ON DELETE SET NULL,
    device_id       TEXT,
    action          TEXT NOT NULL,
    at              INTEGER NOT NULL,
    ip              TEXT
);

CREATE TABLE IF NOT EXISTS integration_health (
    integration     TEXT PRIMARY KEY,          -- 'sms', 'geolocation'
    last_success_at INTEGER,
    last_error      TEXT,
    status          TEXT NOT NULL DEFAULT 'unknown'
);
