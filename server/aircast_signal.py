#!/usr/bin/env python3
"""aircast signalling server.

Pairs two peers on a 6-digit code, buffers the sender's offer until the
receiver arrives, relays ICE candidates both ways, and mints short-lived TURN
REST credentials. Protocol: docs/protocol/signalling.md.

Configuration comes from the environment (systemd EnvironmentFile), so the
shared secret never appears in the unit file or in `ps`:

    AIRCAST_TURN_SECRET   coturn's --static-auth-secret          (required)
    AIRCAST_TURN_URLS     comma-separated turn: URLs             (required)
    AIRCAST_BIND          default 127.0.0.1                      (behind nginx/caddy)
    AIRCAST_PORT          default 8443
    AIRCAST_TTL           pairing + credential lifetime, seconds (default 300)
    AIRCAST_MAX_MISSES    failed joins per IP per minute before refusal (default 10)
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import time
from dataclasses import dataclass, field

import websockets
from websockets.asyncio.server import ServerConnection, serve

log = logging.getLogger("aircast-signal")

CODE = re.compile(r"^[0-9]{6}$")
ROLES = ("sender", "receiver")


def turn_credentials(secret: str, urls: list[str], ttl: int) -> dict:
    """coturn's REST scheme, verbatim from its README:

    usercombo -> "timestamp:username", turn user -> usercombo,
    turn password -> base64(hmac(input_buffer = usercombo, key = shared-secret))

    The credential's lifetime *is* the embedded timestamp. This is unrelated to
    coturn's --stale-nonce, which is the nonce refresh interval.
    """
    username = f"{int(time.time()) + ttl}:{secrets.token_hex(8)}"
    digest = hmac.new(secret.encode(), username.encode(), hashlib.sha1).digest()
    return {
        "urls": urls,
        "username": username,
        "credential": base64.b64encode(digest).decode(),
    }


def client_ip(ws: ServerConnection) -> str:
    """The throttle buckets by client, and in production every connection
    arrives from the reverse proxy on loopback — so one bucket would lock out
    everyone. Trust X-Forwarded-For only from loopback, where the proxy is."""
    peer = ws.remote_address[0] if ws.remote_address else "?"
    if peer in ("127.0.0.1", "::1"):
        forwarded = ws.request.headers.get("X-Forwarded-For") if ws.request else None
        if forwarded:
            return forwarded.split(",")[0].strip()
    return peer


@dataclass
class Pairing:
    """One code, at most one peer per role."""

    created: float = field(default_factory=time.monotonic)
    peers: dict[str, ServerConnection] = field(default_factory=dict)
    # The answerer cannot answer before it has the offer, so hold it.
    offer: dict | None = None


class Server:
    def __init__(self, secret: str, urls: list[str], ttl: int, max_misses: int = 10) -> None:
        self.secret = secret
        self.urls = urls
        self.ttl = ttl
        # A 6-digit code is a guessable space, so the throttle is the defence
        # (issue #6): a client that keeps naming codes nobody is waiting on
        # stops being answered.
        self.max_misses = max_misses
        self.pairings: dict[str, Pairing] = {}
        self.misses: dict[str, list[float]] = {}

    def _throttled(self, ip: str) -> bool:
        now = time.monotonic()
        recent = [t for t in self.misses.get(ip, []) if now - t < 60]
        self.misses[ip] = recent
        return len(recent) >= self.max_misses

    def _miss(self, ip: str) -> None:
        self.misses.setdefault(ip, []).append(time.monotonic())

    async def handle(self, ws: ServerConnection) -> None:
        code: str | None = None
        role: str | None = None
        try:
            async for raw in ws:
                msg = self._decode(raw)
                if msg is None:
                    await self._error(ws, "malformed frame")
                    return

                kind = msg.get("type")
                if kind == "join":
                    if code is not None:
                        await self._error(ws, "already joined")
                        return
                    code, role = await self._join(ws, msg)
                    if code is None:
                        return
                elif code is None:
                    await self._error(ws, "join first")
                    return
                elif kind in ("offer", "answer", "candidate", "bye"):
                    self._relay(code, role, msg)
                # Unknown types are ignored on purpose.
        except websockets.ConnectionClosed:
            pass
        finally:
            if code is not None and role is not None:
                self._leave(code, role)

    async def _join(self, ws: ServerConnection, msg: dict) -> tuple[str | None, str | None]:
        code = msg.get("code")
        role = msg.get("role")
        if not isinstance(code, str) or not CODE.match(code) or role not in ROLES:
            await self._error(ws, "bad join")
            return None, None

        ip = client_ip(ws)
        if self._throttled(ip):
            log.warning("%s is guessing codes; refused", ip)
            await self._error(ws, "too many attempts")
            return None, None

        self._expire()
        # Joining a code nobody is waiting on is what guessing looks like. The
        # sender legitimately arrives first, so only a receiver is charged.
        if role == "receiver" and code not in self.pairings:
            self._miss(ip)
        pairing = self.pairings.setdefault(code, Pairing())
        if role in pairing.peers:
            # Two senders on one code: a typo, or someone shadowing a live
            # pairing. Either way the first peer keeps the slot.
            await self._error(ws, "that role is already taken")
            return None, None
        pairing.peers[role] = ws

        await ws.send(json.dumps({
            "type": "joined",
            "turn": turn_credentials(self.secret, self.urls, self.ttl),
        }))

        other = ROLES[0] if role == ROLES[1] else ROLES[1]
        peer = pairing.peers.get(other)
        if peer is not None:
            # Tell both sides, and flush the buffered offer to a receiver that
            # joined after the sender.
            await asyncio.gather(
                ws.send(json.dumps({"type": "peer"})),
                peer.send(json.dumps({"type": "peer"})),
                return_exceptions=True,
            )
            if role == "receiver" and pairing.offer is not None:
                await ws.send(json.dumps(pairing.offer))
        log.info("code %s: %s joined", code, role)
        return code, role

    def _relay(self, code: str, role: str, msg: dict) -> None:
        pairing = self.pairings.get(code)
        if pairing is None:
            return
        if msg["type"] == "offer" and role == "sender":
            pairing.offer = msg
        other = ROLES[0] if role == ROLES[1] else ROLES[1]
        peer = pairing.peers.get(other)
        if peer is not None:
            # Fire and forget: a dead peer is cleaned up by its own handler.
            asyncio.create_task(self._send(peer, msg))

    @staticmethod
    async def _send(ws: ServerConnection, msg: dict) -> None:
        try:
            await ws.send(json.dumps(msg))
        except websockets.ConnectionClosed:
            pass

    def _leave(self, code: str, role: str) -> None:
        pairing = self.pairings.get(code)
        if pairing is None:
            return
        pairing.peers.pop(role, None)
        if not pairing.peers:
            del self.pairings[code]
        log.info("code %s: %s left", code, role)

    def _expire(self) -> None:
        now = time.monotonic()
        for code, pairing in list(self.pairings.items()):
            if now - pairing.created > self.ttl and len(pairing.peers) < 2:
                for ws in pairing.peers.values():
                    asyncio.create_task(ws.close(code=1000, reason="pairing expired"))
                del self.pairings[code]

    @staticmethod
    def _decode(raw: str | bytes) -> dict | None:
        try:
            msg = json.loads(raw)
        except (ValueError, TypeError):
            return None
        return msg if isinstance(msg, dict) else None

    @staticmethod
    async def _error(ws: ServerConnection, message: str) -> None:
        await Server._send(ws, {"type": "error", "message": message})
        await ws.close(code=1008, reason=message)


async def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    secret = os.environ.get("AIRCAST_TURN_SECRET")
    urls = [u.strip() for u in os.environ.get("AIRCAST_TURN_URLS", "").split(",") if u.strip()]
    if not secret or not urls:
        raise SystemExit("AIRCAST_TURN_SECRET and AIRCAST_TURN_URLS are required")

    server = Server(
        secret,
        urls,
        int(os.environ.get("AIRCAST_TTL", "300")),
        int(os.environ.get("AIRCAST_MAX_MISSES", "10")),
    )
    host = os.environ.get("AIRCAST_BIND", "127.0.0.1")
    port = int(os.environ.get("AIRCAST_PORT", "8443"))
    # TLS is terminated by the reverse proxy; see ops/ for the unit file.
    async with serve(server.handle, host, port):
        log.info("listening on ws://%s:%d", host, port)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
