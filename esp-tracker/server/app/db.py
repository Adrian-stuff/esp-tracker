import sqlite3, threading
from contextlib import contextmanager
from .config import DB_PATH, SCHEMA

_lock = threading.Lock()
_conn: sqlite3.Connection | None = None

def init() -> None:
    global _conn
    _conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    _conn.row_factory = sqlite3.Row
    _conn.executescript(SCHEMA.read_text())
    _conn.commit()

@contextmanager
def cursor():
    """SQLite is single-writer; serialise. Swap for a Postgres pool when the
    prototype outgrows it — the schema is already compatible."""
    assert _conn is not None, "db.init() not called"
    with _lock:
        cur = _conn.cursor()
        try:
            yield cur
            _conn.commit()
        except Exception:
            _conn.rollback()
            raise
        finally:
            cur.close()

def query(sql: str, args: tuple = ()) -> list[sqlite3.Row]:
    with cursor() as c:
        return c.execute(sql, args).fetchall()

def one(sql: str, args: tuple = ()) -> sqlite3.Row | None:
    with cursor() as c:
        return c.execute(sql, args).fetchone()

def execute(sql: str, args: tuple = ()) -> int:
    with cursor() as c:
        c.execute(sql, args)
        return c.lastrowid
