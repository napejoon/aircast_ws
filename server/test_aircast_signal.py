"""Run: pip install websockets pytest pytest-asyncio && pytest server/"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import time

import pytest
import websockets
from websockets.asyncio.server import serve

from aircast_signal import Server, turn_credentials

SECRET = "s3cr3t"
URLS = ["turn:relay.example.com:3478?transport=udp"]


@pytest.fixture
async def endpoint():
    async for url in _endpoint(max_misses=10):
        yield url


@pytest.fixture
async def strict_endpoint():
    async for url in _endpoint(max_misses=2):
        yield url


async def _endpoint(max_misses):
    server = Server(SECRET, URLS, ttl=300, max_misses=max_misses, turn_ttl=43200)
    async with serve(server.handle, "127.0.0.1", 0) as ws_server:
        port = ws_server.sockets[0].getsockname()[1]
        yield f"ws://127.0.0.1:{port}"


async def join(url, code, role, forwarded_for=None):
    headers = {"X-Forwarded-For": forwarded_for} if forwarded_for else None
    ws = await websockets.connect(url, additional_headers=headers)
    await ws.send(json.dumps({"type": "join", "code": code, "role": role}))
    return ws


async def recv(ws):
    return json.loads(await ws.recv())


def test_turn_credentials_match_coturns_construction():
    cred = turn_credentials(SECRET, URLS, ttl=300)
    expiry, _, _ = cred["username"].partition(":")
    assert int(expiry) > time.time()
    expected = hmac.new(SECRET.encode(), cred["username"].encode(), hashlib.sha1).digest()
    assert cred["credential"] == base64.b64encode(expected).decode()


@pytest.mark.asyncio
async def test_the_turn_credential_outlives_the_pairing_window(endpoint):
    # coturn checks the timestamp on every refresh, not only on the first
    # allocation. A credential that died with the 300 s pairing window took the
    # relay allocation down with it five minutes into a working mirror.
    ws = await join(endpoint, "654321", "receiver")
    joined = await recv(ws)
    expiry, _, _ = joined["turn"]["username"].partition(":")
    assert int(expiry) > time.time() + 3600
    await ws.close()


@pytest.mark.asyncio
async def test_offer_is_buffered_for_a_receiver_that_joins_late(endpoint):
    sender = await join(endpoint, "123456", "sender")
    assert (await recv(sender))["type"] == "joined"
    await sender.send(json.dumps({"type": "offer", "sdp": "v=0 offer"}))

    receiver = await join(endpoint, "123456", "receiver")
    assert (await recv(receiver))["type"] == "joined"
    assert (await recv(receiver))["type"] == "peer"
    offer = await recv(receiver)
    assert offer == {"type": "offer", "sdp": "v=0 offer"}

    await receiver.send(json.dumps({"type": "answer", "sdp": "v=0 answer"}))
    assert (await recv(sender))["type"] == "peer"
    assert (await recv(sender))["sdp"] == "v=0 answer"

    await sender.close()
    await receiver.close()


@pytest.mark.asyncio
async def test_candidates_relay_both_ways(endpoint):
    sender = await join(endpoint, "222222", "sender")
    receiver = await join(endpoint, "222222", "receiver")
    for ws in (sender, receiver):
        assert (await recv(ws))["type"] == "joined"
    assert (await recv(sender))["type"] == "peer"
    assert (await recv(receiver))["type"] == "peer"

    await sender.send(json.dumps({"type": "candidate", "candidate": {"sdpMid": "0"}}))
    assert (await recv(receiver))["candidate"] == {"sdpMid": "0"}
    await receiver.send(json.dumps({"type": "candidate", "candidate": {"sdpMid": "1"}}))
    assert (await recv(sender))["candidate"] == {"sdpMid": "1"}

    await sender.close()
    await receiver.close()


@pytest.mark.asyncio
async def test_second_sender_on_the_same_code_is_refused(endpoint):
    first = await join(endpoint, "333333", "sender")
    assert (await recv(first))["type"] == "joined"
    second = await join(endpoint, "333333", "sender")
    assert (await recv(second)) == {"type": "error", "message": "that role is already taken"}
    await first.close()


@pytest.mark.asyncio
@pytest.mark.parametrize("frame", [
    {"type": "join", "code": "12345", "role": "sender"},
    {"type": "join", "code": "123456", "role": "eavesdropper"},
    {"type": "offer", "sdp": "before joining"},
])
async def test_bad_frames_are_refused(endpoint, frame):
    ws = await websockets.connect(endpoint)
    await ws.send(json.dumps(frame))
    assert (await recv(ws))["type"] == "error"
    await ws.close()


@pytest.mark.asyncio
async def test_a_sender_guessing_codes_is_cut_off(strict_endpoint):
    # The sender is the side that types a code it was told. Typing one nobody
    # is waiting on is the only thing that looks like guessing.
    for _ in range(2):
        ws = await join(strict_endpoint, "999999", "sender")
        assert (await recv(ws))["type"] == "joined"
        await ws.close()

    ws = await join(strict_endpoint, "888888", "sender")
    assert (await recv(ws)) == {"type": "error", "message": "too many attempts"}
    await ws.close()


@pytest.mark.asyncio
async def test_a_receiver_arriving_first_is_not_charged_a_miss(strict_endpoint):
    # The receiver invents the code and displays it, so it is always first and
    # its code is never already waiting. Charging it spent the program's own
    # budget on starting up.
    for code in ("111111", "222222", "333333"):
        ws = await join(strict_endpoint, code, "receiver")
        assert (await recv(ws))["type"] == "joined"
        await ws.close()


@pytest.mark.asyncio
async def test_the_throttle_buckets_by_forwarded_client_not_by_the_proxy(strict_endpoint):
    # Two misses from one client must not spend the next client's budget.
    for _ in range(2):
        ws = await join(strict_endpoint, "999999", "sender", forwarded_for="203.0.113.9")
        assert (await recv(ws))["type"] == "joined"
        await ws.close()

    blocked = await join(strict_endpoint, "888888", "sender", forwarded_for="203.0.113.9")
    assert (await recv(blocked))["type"] == "error"
    await blocked.close()

    other = await join(strict_endpoint, "777777", "sender", forwarded_for="198.51.100.4")
    assert (await recv(other))["type"] == "joined"
    await other.close()


@pytest.mark.asyncio
async def test_a_client_cannot_mint_itself_a_fresh_bucket(strict_endpoint):
    # A proxy appends what it saw to whatever the client sent, so a client that
    # sends its own X-Forwarded-For controls the first element and nothing else.
    # Reading that element let one client guess for as long as it liked.
    for i in range(2):
        ws = await join(strict_endpoint, "999999", "sender",
                        forwarded_for=f"10.0.0.{i}, 203.0.113.9")
        assert (await recv(ws))["type"] == "joined"
        await ws.close()

    blocked = await join(strict_endpoint, "888888", "sender",
                         forwarded_for="10.0.0.99, 203.0.113.9")
    assert (await recv(blocked))["type"] == "error"
    await blocked.close()
