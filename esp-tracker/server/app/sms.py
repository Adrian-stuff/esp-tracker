"""SMS transport — used for the escalation ladder and the 'locate now' downlink.

Provider is abstracted because the right choice is regional: for Philippine
numbers a local gateway (Semaphore, Movider) is dramatically cheaper than
Twilio, while Twilio wins on international reach and voice. Verify current
pricing before committing — this is a recurring cost per alert.
"""
import httpx
from .config import (SMS_PROVIDER, SEMAPHORE_API_KEY, SEMAPHORE_SENDER_NAME,
                     TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN, TWILIO_FROM)

class SmsResult:
    def __init__(self, ok: bool, msg_id: str = "", error: str = ""):
        self.ok, self.msg_id, self.error = ok, msg_id, error

async def send(to: str, text: str) -> SmsResult:
    try:
        if SMS_PROVIDER == "semaphore":
            async with httpx.AsyncClient(timeout=15) as c:
                r = await c.post("https://api.semaphore.co/api/v4/messages", data={
                    "apikey": SEMAPHORE_API_KEY, "number": to,
                    "message": text, "sendername": SEMAPHORE_SENDER_NAME})
                r.raise_for_status()
                body = r.json()
                mid = str(body[0].get("message_id", "")) if isinstance(body, list) and body else ""
                return SmsResult(True, mid)

        if SMS_PROVIDER == "twilio":
            async with httpx.AsyncClient(timeout=15) as c:
                r = await c.post(
                    f"https://api.twilio.com/2010-04-01/Accounts/{TWILIO_ACCOUNT_SID}/Messages.json",
                    auth=(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN),
                    data={"To": to, "From": TWILIO_FROM, "Body": text})
                r.raise_for_status()
                return SmsResult(True, r.json().get("sid", ""))

        print(f"[sms:console] -> {to}: {text}", flush=True)
        return SmsResult(True, "console")
    except Exception as e:
        return SmsResult(False, error=str(e))

async def send_locate_command(msisdn: str, secret: str) -> SmsResult:
    """Downlink. The modem raises RI and wakes even from AT+CSCLK sleep, which
    is what makes on-demand location work without holding a TCP socket open
    against carrier NAT."""
    return await send(msisdn, f"LOCATE {secret}")
