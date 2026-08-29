import hashlib, secrets, time
from fastapi import Depends, HTTPException, Request
from argon2 import PasswordHasher
from argon2.exceptions import VerifyMismatchError
from . import db
from .config import SESSION_TTL_HOURS

_ph = PasswordHasher()

def hash_password(p: str) -> str:  return _ph.hash(p)

def verify_password(hash_: str, p: str) -> bool:
    try:
        _ph.verify(hash_, p); return True
    except VerifyMismatchError:
        return False

def _sha(t: str) -> str: return hashlib.sha256(t.encode()).hexdigest()

# ---------------------------------------------------------------- devices ----
def device_token_hash(token: str) -> str: return _sha(token)

async def current_device(request: Request):
    """Per-device bearer. No shared fleet secret: one recovered device must not
    compromise the others."""
    auth = request.headers.get("authorization", "")
    if not auth.startswith("Bearer "):
        raise HTTPException(401, "missing device token")
    row = db.one("SELECT * FROM devices WHERE token_hash=? AND active=1",
                 (_sha(auth[7:]),))
    if not row:
        raise HTTPException(401, "unknown device")
    return row

# ----------------------------------------------------------------- users -----
def create_session(user_id: int) -> str:
    token = secrets.token_urlsafe(32)
    db.execute("INSERT INTO sessions (token_hash,user_id,expires_at) VALUES (?,?,?)",
               (_sha(token), user_id, int(time.time()) + SESSION_TTL_HOURS * 3600))
    return token

async def current_user(request: Request):
    token = request.cookies.get("session") or ""
    if not token:
        auth = request.headers.get("authorization", "")
        if auth.startswith("Bearer "):
            token = auth[7:]
    if not token:
        raise HTTPException(401, "not signed in")
    row = db.one("""SELECT u.* FROM sessions s JOIN users u ON u.id = s.user_id
                    WHERE s.token_hash=? AND s.revoked_at IS NULL AND s.expires_at > ?""",
                 (_sha(token), int(time.time())))
    if not row:
        raise HTTPException(401, "session expired")
    return row

def assert_device_access(user, device_id: str) -> None:
    """Enforced on EVERY device-scoped query. Never trust a client-supplied
    device_id — that is the whole point of the grants table."""
    if user["role"] == "admin":
        return
    if not db.one("SELECT 1 FROM device_access WHERE user_id=? AND device_id=?",
                  (user["id"], device_id)):
        raise HTTPException(403, "no access to this device")

def log_access(user, device_id: str, action: str, ip: str = "") -> None:
    db.execute("INSERT INTO access_log (user_id,device_id,action,at,ip) VALUES (?,?,?,?,?)",
               (user["id"] if user else None, device_id, action, int(time.time()), ip))
