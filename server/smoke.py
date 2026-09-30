"""Pair one receiver and one sender through a running signalling server.

    python smoke.py ws://127.0.0.1:8443

What a cast needs from the server, and nothing more: both joins answered, the
receiver's "joined" carrying a TURN credential, both sides told "peer", and an
offer and an answer crossing. Exit status 0 when all of it happened.

Used by CI against the container image, and on the VPS against a container
started beside the live server before it takes the live port. Code 000000 so
it meets no real receiver; it costs two of the ten joins a minute the throttle
allows one address.
"""

import asyncio
import json
import sys

import websockets

CODE = "000000"


async def join(url: str, role: str):
    ws = await websockets.connect(url)
    await ws.send(json.dumps({"type": "join", "code": CODE, "role": role}))
    return ws


async def expect(ws, kind: str) -> dict:
    msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
    if msg.get("type") != kind:
        raise SystemExit(f"expected {kind!r}, got {msg!r}")
    return msg


async def main(url: str) -> None:
    receiver = await join(url, "receiver")
    turn = (await expect(receiver, "joined")).get("turn") or {}
    if not turn.get("urls") or not turn.get("credential"):
        raise SystemExit(f"joined carried no usable TURN block: {turn!r}")

    sender = await join(url, "sender")
    await expect(sender, "joined")
    await expect(sender, "peer")
    await expect(receiver, "peer")

    await sender.send(json.dumps({"type": "offer", "sdp": "v=0 smoke"}))
    await expect(receiver, "offer")
    await receiver.send(json.dumps({"type": "answer", "sdp": "v=0 smoke"}))
    await expect(sender, "answer")

    await sender.close()
    await receiver.close()
    print(f"ok: paired through {url}, TURN {turn['urls'][0]}")


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8443"))
