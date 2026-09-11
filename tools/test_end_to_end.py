"""End-to-end: two real WebRTC peers pair through the real signalling server
and video actually arrives. Everything but the phone and GStreamer.

    pip install aiortc pytest pytest-asyncio
    pytest tools/ --asyncio-mode=auto
"""

from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import pytest
import websockets
from aiortc import RTCPeerConnection, RTCSessionDescription
from websockets.asyncio.server import serve

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))

from aircast_signal import Server  # noqa: E402
from fake_sender import MovingBarTrack  # noqa: E402

CODE = "424242"


@pytest.fixture
async def endpoint():
    server = Server("s3cr3t", ["turn:relay.example.com:3478"], ttl=300)
    async with serve(server.handle, "127.0.0.1", 0) as ws_server:
        yield f"ws://127.0.0.1:{ws_server.sockets[0].getsockname()[1]}"


async def run_sender(url: str, ready: asyncio.Event) -> None:
    async with websockets.connect(url) as ws:
        await ws.send(json.dumps({"type": "join", "code": CODE, "role": "sender"}))
        pc = RTCPeerConnection()
        pc.addTrack(MovingBarTrack(width=320, height=240))
        try:
            async for raw in ws:
                msg = json.loads(raw)
                if msg["type"] == "peer":
                    offer = await pc.createOffer()
                    await pc.setLocalDescription(offer)
                    await ws.send(json.dumps({"type": "offer", "sdp": pc.localDescription.sdp}))
                elif msg["type"] == "answer":
                    await pc.setRemoteDescription(
                        RTCSessionDescription(sdp=msg["sdp"], type="answer")
                    )
                    await ready.wait()
                    return
        finally:
            await pc.close()


async def run_receiver(url: str, frames: asyncio.Queue) -> None:
    async with websockets.connect(url) as ws:
        await ws.send(json.dumps({"type": "join", "code": CODE, "role": "receiver"}))
        pc = RTCPeerConnection()

        @pc.on("track")
        def on_track(track):
            async def drain():
                for _ in range(3):
                    await frames.put(await track.recv())

            asyncio.ensure_future(drain())

        try:
            async for raw in ws:
                msg = json.loads(raw)
                if msg["type"] == "offer":
                    await pc.setRemoteDescription(
                        RTCSessionDescription(sdp=msg["sdp"], type="offer")
                    )
                    answer = await pc.createAnswer()
                    await pc.setLocalDescription(answer)
                    await ws.send(json.dumps({"type": "answer", "sdp": pc.localDescription.sdp}))
                    await asyncio.sleep(10)
                    return
        finally:
            await pc.close()


@pytest.mark.asyncio
async def test_video_reaches_a_receiver_that_joins_after_the_sender(endpoint):
    frames: asyncio.Queue = asyncio.Queue()
    ready = asyncio.Event()

    sender = asyncio.ensure_future(run_sender(endpoint, ready))
    # The sender is deliberately first, so the offer is served from the
    # server's buffer rather than relayed live.
    await asyncio.sleep(0.3)
    receiver = asyncio.ensure_future(run_receiver(endpoint, frames))

    first = await asyncio.wait_for(frames.get(), timeout=20)
    second = await asyncio.wait_for(frames.get(), timeout=20)
    assert first.width == 320 and first.height == 240
    # The picture moves, so two decoded frames are not the same picture.
    assert first.to_ndarray(format="bgr24").tobytes() != second.to_ndarray(format="bgr24").tobytes()

    ready.set()
    for task in (sender, receiver):
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
