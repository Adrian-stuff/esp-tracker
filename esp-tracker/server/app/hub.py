"""WebSocket fan-out to open dashboards."""
import json
from fastapi import WebSocket

class Hub:
    def __init__(self):
        self._clients: set[WebSocket] = set()

    async def join(self, ws: WebSocket):
        await ws.accept(); self._clients.add(ws)

    def leave(self, ws: WebSocket):
        self._clients.discard(ws)

    async def broadcast(self, kind: str, payload: dict):
        dead = []
        msg = json.dumps({"kind": kind, "payload": payload})
        for ws in list(self._clients):
            try:
                await ws.send_text(msg)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self._clients.discard(ws)

hub = Hub()
