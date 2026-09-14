# Signalling wire protocol (v0)

Defined here because the sender needed it first; the server
(`ops/aircast-signal.service` → `/usr/local/bin/aircast-signal`) must implement
this side of it. Derived from `docs/research/signaling-turn-hosting.md` §3 and §7.

Transport: one WebSocket, TLS in production. One JSON object per frame, every
frame carries `type`. Unknown types are ignored, not errors — that is what makes
adding a type later cheap.

The **sender is always the offerer**, so the receiver never has to decide who
goes first.

## Frames

| Direction | Frame |
|---|---|
| client → server | `{"type":"join","code":"123456","role":"sender"\|"receiver"}` |
| server → client | `{"type":"joined","turn":{"urls":[...],"username":"...","credential":"..."}}` |
| server → client | `{"type":"peer"}` — the other side has joined this code |
| sender → server → receiver | `{"type":"offer","sdp":"..."}` |
| receiver → server → sender | `{"type":"answer","sdp":"..."}` |
| both ways | `{"type":"candidate","candidate":{"candidate":"...","sdpMid":"...","sdpMLineIndex":0}}` |
| both ways | `{"type":"bye"}` |
| server → client | `{"type":"error","message":"..."}` |

## Server obligations

1. Pair the two peers on the 6-digit code.
2. **Buffer the offer** until the receiver joins. A peer that joins second must
   still get the offer that was sent before it arrived; forward-and-forget is a
   bug.
3. Relay candidates as opaque blobs in both directions, immediately (trickle).
4. Mint TURN REST credentials per pairing and send them in `joined`:
   `username = "<expiry-unix>:<opaque-id>"`,
   `credential = base64(HMAC-SHA1(username, static-auth-secret))`, with the
   shared secret read from `/etc/aircast/signal.env`. Expiry must outlive the
   longest *session*, not the pairing window (`AIRCAST_TURN_TTL`, default 12 h):
   coturn re-checks the timestamp on every allocation and permission refresh,
   so a credential tied to the 300 s pairing window killed the relay
   allocation five minutes into a working mirror. It is unrelated to coturn's
   `stale-nonce`.
5. Expire the code and drop the buffered offer on connect or on a short TTL.

## Client obligations

- ICE is `relay`-only. The sender sets `iceTransportPolicy: 'relay'`, so the
  TURN entry from `joined` is the whole ICE configuration and a swap to a
  managed TURN is a server-side change only.
- Codec preference is H.264 then VP8. VP8 is not optional: libwebrtc's Android
  AAR ships no software H.264, so a MediaTek or Unisoc phone has no H.264
  encoder at all.

## Abuse

A 6-digit code is a guessable space, so the code is not a secret — the server
is the defence. It charges a *miss* to a **sender** that joins a code no
receiver is waiting on, and refuses a client past `AIRCAST_MAX_MISSES`
(default 10) misses per minute.

The role matters and was once written the other way round. The receiver invents
the code and displays it, so the receiver is always first and its code is never
"already waiting" — charging it meant every ordinary start of the program spent
one of its own ten attempts, while the sender, the only side that can type a
code it does not know, was never charged at all.

Behind a reverse proxy every connection arrives from loopback, so the server
trusts `X-Forwarded-For` **only** when the peer address is loopback, and reads
the **last** element of it. A proxy appends what it saw to whatever the client
sent, so the first element is the client's own string: reading that let anyone
mint a fresh bucket per request and guess without limit. The proxy must also
overwrite the header rather than append to it
(`ops/nginx-aircast-signal.conf.template` does); without the header at all, the
whole internet would share one bucket.
