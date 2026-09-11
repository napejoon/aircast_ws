#!/usr/bin/env python3
"""A phone-shaped sender, for testing the receiver without a phone.

Speaks the same protocol as `sender/` (docs/protocol/signalling.md): joins a
code as the sender, waits for the receiver, offers one video track. The picture
is a moving bar with a frame counter, so a frozen or duplicated stream is
visible at a glance.

    pip install aiortc
    python tools/fake_sender.py --signal ws://127.0.0.1:8443 --code 123456

By default it lets aiortc use whatever ICE it can find, which is direct
host-to-host on one machine. Pass --relay to honour the TURN credentials the
server mints, which is what the real sender always does.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import time

import websockets
from aiortc import (
    RTCConfiguration,
    RTCIceServer,
    RTCPeerConnection,
    RTCSessionDescription,
    VideoStreamTrack,
)
from av import VideoFrame
import numpy as np


class MovingBarTrack(VideoStreamTrack):
    """A moving bar on a dark field, 30 fps."""

    def __init__(self, width: int = 1280, height: int = 720) -> None:
        super().__init__()
        self.width = width
        self.height = height
        self.count = 0

    async def recv(self) -> VideoFrame:
        pts, time_base = await self.next_timestamp()

        image = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        image[:, :, 2] = 24
        x = (self.count * 8) % self.width
        image[:, x : x + 64] = 220
        # A per-frame stripe pattern at the top, so a stalled stream is obvious
        # even without reading a counter.
        image[0:16, : (self.count % self.width)] = 180

        frame = VideoFrame.from_ndarray(image, format="bgr24")
        frame.pts = pts
        frame.time_base = time_base
        self.count += 1
        return frame


async def cast(signal_url: str, code: str, relay: bool, seconds: float) -> None:
    async with websockets.connect(signal_url) as ws:
        await ws.send(json.dumps({"type": "join", "code": code, "role": "sender"}))

        pc: RTCPeerConnection | None = None
        track = MovingBarTrack()
        deadline = time.monotonic() + seconds if seconds else None

        async for raw in ws:
            msg = json.loads(raw)
            kind = msg.get("type")

            if kind == "joined":
                turn = msg["turn"]
                config = RTCConfiguration([
                    RTCIceServer(
                        urls=turn["urls"],
                        username=turn["username"],
                        credential=turn["credential"],
                    )
                ]) if relay else RTCConfiguration([])
                pc = RTCPeerConnection(config)
                pc.addTrack(track)

            elif kind == "peer" and pc is not None:
                offer = await pc.createOffer()
                await pc.setLocalDescription(offer)
                # aiortc has no trickle ICE: by the time setLocalDescription
                # returns, gathering is complete and the SDP carries every
                # candidate. The server relays candidates for peers that do
                # trickle; this one simply never sends any.
                await ws.send(json.dumps({
                    "type": "offer",
                    "sdp": pc.localDescription.sdp,
                }))

            elif kind == "answer" and pc is not None:
                await pc.setRemoteDescription(
                    RTCSessionDescription(sdp=msg["sdp"], type="answer")
                )
                print("answered; sending frames")
                while deadline is None or time.monotonic() < deadline:
                    await asyncio.sleep(0.5)
                    if pc.connectionState in ("failed", "closed"):
                        print(f"connection {pc.connectionState}")
                        break
                await pc.close()
                return

            elif kind in ("bye", "error"):
                print(msg.get("message", "the receiver hung up"))
                if pc:
                    await pc.close()
                return


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--signal", required=True, help="signalling server URL")
    parser.add_argument("--code", required=True, help="6-digit pairing code")
    parser.add_argument("--relay", action="store_true", help="use the minted TURN credentials")
    parser.add_argument("--seconds", type=float, default=0, help="stop after N seconds (0 = forever)")
    args = parser.parse_args()

    asyncio.run(cast(args.signal, args.code, args.relay, args.seconds))


if __name__ == "__main__":
    main()
