# Kagami signalling server

One file, one WebSocket, no database. Protocol: `docs/protocol/signalling.md`.
It pairs two peers on a 6-digit code, buffers the offer, relays candidates and
mints TURN REST credentials — nothing else, per
`docs/research/signaling-turn-hosting.md` §7.

## Run locally

```bash
pip install -r requirements.txt
AIRCAST_TURN_SECRET=dev AIRCAST_TURN_URLS=turn:127.0.0.1:3478 python aircast_signal.py
pytest                            # 15 tests, no network needed
```

## Deploy

TLS is terminated by nginx or caddy in front; the server binds 127.0.0.1 only.

```bash
install -d /opt/aircast && python3 -m venv /opt/aircast/venv
/opt/aircast/venv/bin/pip install websockets
install -m 755 aircast_signal.py /opt/aircast/aircast_signal.py
printf '#!/bin/sh\nexec /opt/aircast/venv/bin/python /opt/aircast/aircast_signal.py\n' \
  > /usr/local/bin/aircast-signal && chmod 755 /usr/local/bin/aircast-signal
install -o root -g aircast -m 640 /dev/null /etc/aircast/signal.env
systemctl enable --now aircast-signal
```

`/etc/aircast/signal.env` — the shared secret lives here and nowhere else, and
is the same string as coturn's `static-auth-secret`:

```
AIRCAST_TURN_SECRET=<coturn static-auth-secret>
AIRCAST_TURN_URLS=turn:relay.example.com:3478?transport=udp,turns:relay.example.com:5349?transport=tcp
AIRCAST_PORT=8443
AIRCAST_TTL=300          # pairing code
AIRCAST_TURN_TTL=43200   # TURN credential: must outlive the longest session, not the code
```

`ProtectSystem=strict` in the unit means `/opt/aircast` is read-only to the
service, which is what we want — it writes nothing.

## Deploy with Docker

The same server, same `/etc/aircast/signal.env`, same `127.0.0.1:8443`; nginx
and coturn stay on the host untouched. `compose.yaml` says why it uses the
host's network rather than a port mapping — a mapped port would make the join
throttle count every user as one address.

From a checkout, copy the four files the image needs:

```bash
ssh vps mkdir -p /opt/aircast/docker
scp server/{Dockerfile,compose.yaml,.dockerignore,aircast_signal.py,smoke.py} vps:/opt/aircast/docker/
```

On the VPS — build, then try the image **beside** the live server first, on
8453, where nothing routes to it:

```bash
cd /opt/aircast/docker
docker compose build
docker run -d --rm --name kagami-signal-try --network host --read-only --cap-drop ALL \
  --env-file /etc/aircast/signal.env -e AIRCAST_PORT=8453 kagami-signal
/opt/aircast/venv/bin/python smoke.py ws://127.0.0.1:8453     # ok: paired through ...
docker stop kagami-signal-try
```

Then swap. The gap between the two lines is the only downtime, about a second:

```bash
systemctl disable --now aircast-signal && docker compose up -d
/opt/aircast/venv/bin/python smoke.py ws://127.0.0.1:8443
docker logs --tail 20 kagami-signal
```

Rollback, if a real cast then fails:

```bash
docker compose down && systemctl enable --now aircast-signal
```

Logs are `docker logs kagami-signal` now, not `journalctl -u aircast-signal`.
An update is the `scp` above, then `docker compose up -d --build`.
