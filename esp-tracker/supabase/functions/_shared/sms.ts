// SMS transport. Regional choice: for Philippine numbers a local gateway
// (Semaphore, Movider) is dramatically cheaper than Twilio. Verify current
// pricing before committing — this is a recurring cost per alert.

export interface SmsResult { ok: boolean; msgId?: string; error?: string }

const PROVIDER = Deno.env.get("SMS_PROVIDER") ?? "console";

export async function sendSms(to: string, text: string): Promise<SmsResult> {
  try {
    if (PROVIDER === "semaphore") {
      const r = await fetch("https://api.semaphore.co/api/v4/messages", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: new URLSearchParams({
          apikey: Deno.env.get("SEMAPHORE_API_KEY") ?? "",
          number: to,
          message: text,
          sendername: Deno.env.get("SEMAPHORE_SENDER_NAME") ?? "",
        }),
      });
      if (!r.ok) return { ok: false, error: `semaphore ${r.status}` };
      const body = await r.json();
      return { ok: true, msgId: String(body?.[0]?.message_id ?? "") };
    }

    if (PROVIDER === "twilio") {
      const sid = Deno.env.get("TWILIO_ACCOUNT_SID") ?? "";
      const r = await fetch(
        `https://api.twilio.com/2010-04-01/Accounts/${sid}/Messages.json`, {
        method: "POST",
        headers: {
          Authorization: "Basic " + btoa(`${sid}:${Deno.env.get("TWILIO_AUTH_TOKEN")}`),
          "Content-Type": "application/x-www-form-urlencoded",
        },
        body: new URLSearchParams({ To: to, From: Deno.env.get("TWILIO_FROM") ?? "", Body: text }),
      });
      if (!r.ok) return { ok: false, error: `twilio ${r.status}` };
      return { ok: true, msgId: (await r.json()).sid };
    }

    console.log(`[sms:console] -> ${to}: ${text}`);
    return { ok: true, msgId: "console" };
  } catch (e) {
    return { ok: false, error: String(e) };
  }
}

// The downlink. The modem raises RI and wakes even from AT+CSCLK sleep, which
// is what makes "locate now" work without holding a TCP socket open against
// carrier NAT.
export async function sendLocateCommand(msisdn: string): Promise<SmsResult> {
  return sendSms(msisdn, `LOCATE ${Deno.env.get("SMS_CMD_SECRET") ?? ""}`);
}

export function sosText(
  who: string,
  loc: { lat: number; lon: number; accuracy_m: number; source: string } | null,
) {
  return loc
    ? `SOS from ${who}. https://maps.google.com/?q=${loc.lat.toFixed(5)},${loc.lon.toFixed(5)} ` +
      `(+/-${Math.round(loc.accuracy_m)}m, ${loc.source})`
    : `SOS from ${who}. Position not yet known.`;
}
