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
    AIRCAST_TTL           pairing-code lifetime, seconds                (default 300)
    AIRCAST_TURN_TTL      TURN credential lifetime, seconds             (default 43200)
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
    everyone. Trust X-Forwarded-For only from loopback, where the proxy is.

    The LAST element, not the first. A proxy appends what it saw to whatever the
    client sent, so the first element is attacker-controlled: reading it let
    anyone mint a fresh bucket per request and walk the 6-digit space with the
    throttle — the only defence this design has — switched off. The proxy config
    overwrites the header for the same reason; this is the half that survives a
    proxy someone else configures."""
    peer = ws.remote_address[0] if ws.remote_address else "?"
    if peer in ("127.0.0.1", "::1"):
        # get_all, not get: a field that arrives on two lines means exactly what
        # the same field comma-joined on one line means (RFC 9110), but get() is
        # Mapping.get and only swallows KeyError, while websockets raises
        # MultipleValuesError -- a LookupError that is not a KeyError. A second
        # proxy that adds its own line rather than appending to ours therefore
        # took the connection down with an unhandled exception and a 1011
        # instead of being read. The last element of the last line is the same
        # rule as before: the nearest proxy's word for who this is.
        forwarded = ws.request.headers.get_all("X-Forwarded-For") if ws.request else []
        if forwarded:
            return forwarded[-1].split(",")[-1].strip()
    return peer


async def _still_there(ws: ServerConnection) -> bool:
    """Is anyone actually on the other end of this socket?

    A peer that walked out of range sends no close frame and no FIN, so the
    socket stays open on this side and the only way to find out is to ask and
    wait a moment for the answer.
    """
    try:
        await asyncio.wait_for(await ws.ping(), timeout=2)
    except (websockets.ConnectionClosed, asyncio.TimeoutError, OSError):
        return False
    return True


@dataclass
class Pairing:
    """One code, at most one peer per role."""

    created: float = field(default_factory=time.monotonic)
    peers: dict[str, ServerConnection] = field(default_factory=dict)
    # The answerer cannot answer before it has the offer, so hold it.
    offer: dict | None = None


class Server:
    def __init__(
        self, secret: str, urls: list[str], ttl: int, max_misses: int = 10, turn_ttl: int = 43200
    ) -> None:
        self.secret = secret
        self.urls = urls
        self.ttl = ttl
        # Not the pairing TTL. The timestamp in a REST username is checked by
        # coturn on every authenticated request, not only the first: the
        # allocation refresh and the permission refresh both carry it, and the
        # peers keep sending those for as long as the screen is mirrored. With
        # the two lifetimes shared, a credential minted at join expired five
        # minutes later, the next refresh got 401, coturn dropped the
        # allocation, and the picture froze mid-session. This has to outlive the
        # longest session anyone will sit through, and no more than that: it is
        # also how long a leaked credential can burn relay bandwidth.
        self.turn_ttl = turn_ttl
        # A 6-digit code is a guessable space, so the throttle is the defence
        # (issue #6): a client that keeps naming codes nobody is waiting on
        # stops being answered.
        self.max_misses = max_misses
        self.pairings: dict[str, Pairing] = {}
        self.misses: dict[str, list[float]] = {}

    def _throttled(self, ip: str) -> bool:
        now = time.monotonic()
        recent = [t for t in self.misses.get(ip, []) if now - t < 60]
        if recent:
            self.misses[ip] = recent
        else:
            # An address with nothing recent is an address with no history, and
            # keeping the empty list is what turned this into a dict that only
            # ever grows: one entry per address that ever guessed wrong, for the
            # life of the process, on a server that is meant to run for months.
            self.misses.pop(ip, None)
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
                self._leave(code, role, ws)

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
        # Every join costs, not only the blind one. Charging the side that has
        # to guess left the door open on the side that does not: a receiver's
        # join was never a miss by the old rule, so the whole six-digit space
        # could be walked at no cost -- "that role is already taken" for a code
        # somebody is really waiting on, "joined" for the rest, which is exactly
        # the oracle the throttle exists to deny. A join is also what mints a
        # TURN credential, so a client that guessed nothing at all could sit
        # there collecting twelve-hour relay credentials until coturn had no
        # quota left for a real cast. The cost to an honest receiver is one
        # attempt per start of the program, out of ten a minute.
        self._miss(ip)
        pairing = self.pairings.setdefault(code, Pairing())
        incumbent = pairing.peers.get(role)
        if incumbent is not None and not await _still_there(incumbent):
            # The peer holding this role is gone and has not noticed. A device
            # that walks out of Wi-Fi sends no close frame and no FIN, so its
            # socket stays open on this side until a write finally fails, and
            # the same device coming back on another network was refused by its
            # own corpse -- with the only code it has, the one on its screen.
            # Asking costs one ping and two seconds, and only when the role
            # looks taken.
            log.info("code %s: replacing a %s that stopped answering", code, role)
            del pairing.peers[role]
            incumbent = None
        if incumbent is not None:
            # Two senders on one code: a typo, or someone shadowing a live
            # pairing. Either way the first peer keeps the slot.
            await self._error(ws, "that role is already taken")
            return None, None
        pairing.peers[role] = ws

        await ws.send(json.dumps({
            "type": "joined",
            "turn": turn_credentials(self.secret, self.urls, self.turn_ttl),
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

    def _leave(self, code: str, role: str, ws: ServerConnection) -> None:
        pairing = self.pairings.get(code)
        # The pairing filed under this code is not necessarily the one this
        # connection joined. A receiver whose network vanishes leaves a socket
        # nobody has closed yet; it comes back a second later with the same code,
        # because it only ever has the one it printed on screen, and the new
        # connection takes the slot. The old handler then finishes dying when the
        # close handshake it will never get an answer to times out, and popping
        # the role blind evicted the live connection that had replaced it.
        # Nothing told that receiver, because nothing was wrong with its socket,
        # and a socket that stays open never reconnects -- so it sat there
        # showing its code with the server no longer able to reach it, which is
        # the failure the reconnect in receiver/main.c was written to end. Leave
        # only if this connection is still the one holding the role.
        if pairing is None or pairing.peers.get(role) is not ws:
            return
        del pairing.peers[role]
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
        turn_ttl=int(os.environ.get("AIRCAST_TURN_TTL", "43200")),
    )
    host = os.environ.get("AIRCAST_BIND", "127.0.0.1")
    port = int(os.environ.get("AIRCAST_PORT", "8443"))
    # TLS is terminated by the reverse proxy; see ops/ for the unit file.
    async with serve(server.handle, host, port):
        log.info("listening on ws://%s:%d", host, port)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
