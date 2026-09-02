// SMS format parser for tracker firmware messages.
//
// Two formats are supported:
// 1. LOC — routine position report
// 2. WIFISCAN — WiFi BSSID scan for server-side place matching
//
// Both use truncated-SHA256-over-a-shared-secret verification.
// Ported from server/app/tracker_sms.py — keep them in sync.

const SOURCE_CODES: Record<string, string> = {
  g: "gnss", w: "wifi", b: "ble_anchor", c: "cell",
};

// Battery percent is OPTIONAL in the regex: a device on firmware from
// before battery_pct was added to the wire format still sends the 5-field
// form and must keep parsing after this deploys, ahead of every tracker in
// the field getting reflashed. New firmware always sends the 6-field form.
const LOC_RE =
  /^LOC ([gwbc]),(-?\d+\.\d+),(-?\d+\.\d+),(\d+(?:\.\d+)?),(\d+)(?:,(\d{1,3}))?,([0-9a-f]{8})$/;

async function sha256Hex(s: string): Promise<string> {
  const buf = await crypto.subtle.digest(
    "SHA-256",
    new TextEncoder().encode(s),
  );
  return [...new Uint8Array(buf)]
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

async function code(secret: string, payload: string): Promise<string> {
  return (await sha256Hex(secret + payload)).slice(0, 8);
}

export interface LocParse {
  source: string;
  lat: number;
  lon: number;
  accuracy_m: number;
  recorded_at: number;
  // undefined for the older 5-field wire format (a device not yet
  // reflashed past the battery_pct addition).
  battery_pct?: number;
}

export interface WifiAp {
  bssid: string;
  rssi: number;
  ssid: string;
}

export interface WifiScanParse {
  recorded_at: number;
  aps: WifiAp[];
}

/**
 * Parse and verify a LOC report.
 * Returns structured data for a validly formatted AND correctly-coded
 * LOC report, else null.
 */
export async function parseLoc(
  text: string,
  secret: string,
): Promise<LocParse | null> {
  const m = LOC_RE.exec(text);
  if (!m) return null;

  const [, src, latS, lonS, accS, epochS, battS, hexCode] = m;
  // The hash covers exactly the fields the device actually sent — the
  // payload string must match byte-for-byte what report.cpp hashed, so
  // battery's presence/absence changes which payload we reconstruct here.
  let payload = `${src},${latS},${lonS},${accS},${epochS}`;
  if (battS !== undefined) payload += `,${battS}`;

  if ((await code(secret, payload)) !== hexCode) return null;

  return {
    source: SOURCE_CODES[src] ?? "cell",
    lat: parseFloat(latS),
    lon: parseFloat(lonS),
    accuracy_m: parseFloat(accS),
    recorded_at: parseInt(epochS, 10),
    battery_pct: battS !== undefined ? parseInt(battS, 10) : undefined,
  };
}

/**
 * Parse and verify a WIFISCAN report.
 * Returns structured data for a validly formatted AND correctly-coded
 * WIFISCAN report, else null.
 */
export async function parseWifiScan(
  text: string,
  secret: string,
): Promise<WifiScanParse | null> {
  if (!text.startsWith("WIFISCAN ")) return null;
  const body = text.slice("WIFISCAN ".length);

  // Split off the trailing code (last 9 chars: comma + 8 hex)
  if (body.length < 10) return null;
  const splitIdx = body.lastIndexOf(",");
  if (splitIdx < 0) return null;

  const payloadPart = body.slice(0, splitIdx);
  const hexCode = body.slice(splitIdx + 1);

  if (hexCode.length !== 8 || !/^[0-9a-f]{8}$/.test(hexCode)) return null;
  if ((await code(secret, payloadPart)) !== hexCode) return null;

  // Parse the payload: <epoch>,<bssid>:<rssi>:<ssid>,...
  const commaIdx = payloadPart.indexOf(",");
  if (commaIdx < 0) return null;

  const recordedAt = parseInt(payloadPart.slice(0, commaIdx), 10);
  if (isNaN(recordedAt)) return null;

  const apsPart = payloadPart.slice(commaIdx + 1);
  const aps: WifiAp[] = [];

  for (const apStr of apsPart.split(",")) {
    // Format: AA:BB:CC:DD:EE:FF:<rssi>:<ssid>
    const segments = apStr.split(":", 7);
    if (segments.length < 7) continue;

    const bssid = segments.slice(0, 6).join(":");
    const rssi = parseInt(segments[6], 10);
    if (isNaN(rssi)) continue;
    const ssid = segments.length > 7 ? segments[7] : "";

    aps.push({ bssid, rssi, ssid });
  }

  if (!aps.length) return null;
  return { recorded_at: recordedAt, aps };
}
