# Provisioning the signalling + TURN host

Runbook for [#14](https://github.com/napejoon/aircast_ws/issues/14). Decisions it implements come from
[#5](https://github.com/napejoon/aircast_ws/issues/5) (self-host coturn + our own signalling, Singapore VPS)
and [#7](https://github.com/napejoon/aircast_ws/issues/7) (signalling server is a Rust binary).

**Scope: coturn, the domain, TLS and the firewall — everything [#13](https://github.com/napejoon/aircast_ws/issues/13)'s
spike needs for a working relay.** The signalling server's unit file is included as a template but cannot be
deployed until that binary exists.

Every command here was checked against the coturn Ubuntu actually ships (4.5.2 on 22.04, 4.6.1 on 24.04) and
against Debian's packaging — not against coturn's master docs, which document options these builds lack. Where
the two differ, the note says so. Several steps exist only because of a packaging quirk; none of them are
decoration.

## 0. What only you can do

Buying the VPS, pointing the domain and holding the credentials are yours. Everything after that is mechanical.

- [ ] VPS in **Singapore**. Shortlist from #5: **Vultr** Cloud Compute ($6/mo, 2 TB pooled, $0.01/GB over)
      or **Hetzner** CX22 in SGP. **Check Hetzner's Singapore page before buying** — its allowance there is
      "0.5 TB to 5 TB" by plan, not the 20 TB of its EU regions, and overage is $8.49/TB.
- [ ] Ubuntu LTS image (22.04 or 24.04), smallest plan — this box relays bytes, it does not transcode.
- [ ] A domain or subdomain (e.g. `turn.example.com`) with an **A record pointing at the VPS's IPv4**.
- [ ] A password-manager entry for the shared secret. Never a file in this repo.

**Run the whole session with shell history off.** The secret must not end up in `~/.bash_history`, and
`/proc/<pid>/cmdline` is world-readable, so it must never appear as a command argument either — the steps
below are written to keep it out of both.

```bash
set +o history; unset HISTFILE
```

Record the answers under **Provisioned facts** at the bottom, and mirror them into #14.

## 1. Base box

The Debian package starts coturn on install, with its own near-empty config and the admin CLI listening. Mask
it first so it never serves traffic with a config that is not ours:

```bash
# as root on the fresh VPS
apt update && apt upgrade -y
systemctl mask coturn          # before apt can start it
apt install -y coturn certbot
systemctl is-enabled coturn    # expect: masked
ss -lntup | grep -E '3478|5349|5766'   # expect: no output

id turnserver                  # the package creates this user
turnserver --version           # note it in Provisioned facts — it picks the README that governs the config

# unprivileged user for the signalling binary (deployed later)
useradd --system --no-create-home --shell /usr/sbin/nologin aircast
```

The `5766` in that check is not incidental: the package default leaves coturn's **admin CLI on 127.0.0.1:5766
with an empty password**. Our config turns it off with `no-cli`.

## 2. Certificate

**Before running certbot:** if the provider has its own cloud firewall in front of the box (Vultr firewall
groups, Hetzner Cloud Firewalls — both opt-in, but deny all inbound once attached), open **22/tcp and 80/tcp
there now**. `certonly --standalone` needs inbound TCP/80 from Let's Encrypt's validators. The host firewall
is still inactive at this point, so a timeout here is the cloud one.

```bash
# the A record must already resolve to this box, or validation times out
dig +short turn.example.com; curl -fsS https://ifconfig.me; echo
```

```bash
certbot certonly --standalone -d turn.example.com --agree-tos -m you@example.com --no-eff-email
```

coturn drops to the `turnserver` user, so it must be able to read the key. Use certbot's documented mechanism,
not ACLs: certbot promises that changes to the **group owner and group mode** of `privkey.pem` "will be
preserved on renewals" — a `setfacl` entry is not preserved, because each renewal writes a brand-new
`privkeyN.pem` that inherits nothing.

```bash
chmod 0755 /etc/letsencrypt/live /etc/letsencrypt/archive
chgrp turnserver /etc/letsencrypt/archive/turn.example.com/privkey*.pem
chmod 0640       /etc/letsencrypt/archive/turn.example.com/privkey*.pem
```

Prove both today's read and the renewal path — the first alone passes on day 1 and fails 60 days later:

```bash
sudo -u turnserver test -r /etc/letsencrypt/live/turn.example.com/privkey.pem && echo readable
certbot renew --dry-run
```

**The renewal problem, stated plainly:** coturn reads the certificate once at start and has no SIGHUP reload
(its own issues #725 and #1446 are still open). A renewal therefore needs a restart, and a restart drops every
live TURN allocation — it cuts any mirror session in flight. Decide the policy now and write it into #14:
accept a ~90-day interruption at a known hour, or add a hook that restarts only when no allocations are active.

```bash
cat >/etc/letsencrypt/renewal-hooks/deploy/coturn.sh <<'EOF'
#!/bin/sh
systemctl restart coturn
EOF
chmod +x /etc/letsencrypt/renewal-hooks/deploy/coturn.sh
```

## 3. coturn config

Generate the secret straight into a root-owned file. It must not pass through a command argument, because
`/proc/<pid>/cmdline` is world-readable and `turnserver` is a local account:

```bash
install -d -o root -g aircast -m 0750 /etc/aircast
( umask 077; openssl rand -hex 32 >/etc/aircast/turn.secret )
chgrp aircast /etc/aircast/turn.secret
chmod 0640    /etc/aircast/turn.secret
```

Copy `ops/turnserver.conf.template` from this repo to `/etc/turnserver.conf` and fill in the two plain
placeholders by hand:

| Placeholder | Value |
|---|---|
| `<VPS_PUBLIC_IPV4>` | the VPS address (`ip -4 addr show` / provider panel) — appears twice, as `listening-ip` and as a `denied-peer-ip` |
| `<TURN_DOMAIN>` | `turn.example.com` |

Then substitute the secret in-process, so it never becomes an argv entry (`sed -i "s/.../$SECRET/"` would
leak it — the script is an argument):

```bash
python3 - <<'EOF'
import pathlib
s = pathlib.Path('/etc/aircast/turn.secret').read_text().strip()
p = pathlib.Path('/etc/turnserver.conf')
p.write_text(p.read_text().replace('<SHARED_SECRET_HEX>', s))
EOF
grep -c '<SHARED_SECRET_HEX>\|<VPS_PUBLIC_IPV4>\|<TURN_DOMAIN>' /etc/turnserver.conf   # expect: 0
```

Confirm every option in the file exists in the installed build. **Use `-h`, not `--help`:** `--help` reaches
coturn's option handler as `case 'h': break;` and then falls through to full startup, binding the listeners —
only the literal `-h` is caught by the early argv scan.

```bash
help=$(turnserver -h)
for opt in listening-port tls-listening-port listening-ip external-ip min-port max-port \
           use-auth-secret static-auth-secret realm stale-nonce cert pkey \
           denied-peer-ip no-multicast-peers no-tcp-relay proc-user proc-group no-cli \
           max-bps bps-capacity total-quota user-quota log-file; do
  printf '%s\n' "$help" | grep -qE -- "(^|[[:space:],])--$opt([[:space:]=,]|$)" || echo "MISSING: $opt"
done
```

Any `MISSING:` line means coturn will **silently ignore** that option, not reject it — it logs
`Bad configuration format: <line>` at WARNING and starts anyway. Remove or replace the line first.

File ownership needs no action: `debian/coturn.postinst` already applies
`dpkg-statoverride --add root turnserver 640 /etc/turnserver.conf`. Verify rather than re-set it:

```bash
stat -c '%U %G %a' /etc/turnserver.conf   # expect: root turnserver 640
```

**Nothing to do in `/etc/default/coturn`.** `TURNSERVER_ENABLED` is read only by `/etc/init.d/coturn`; the
shipped systemd unit has no `EnvironmentFile=` and no condition referencing it, so it starts regardless.
(README.Debian still tells you to set it — that text predates the systemd unit.) Masking the service in
section 1 is what actually kept it down.

Now the drop-in that keeps coturn alive across reboots:

```bash
# Debian #998680: the shipped unit is After=network.target only, so coturn loses
# the boot race against listening-ip= and dies with "Cannot configure any
# meaningful IP listener address". Restart=on-failure with no RestartSec then
# burns the start limit (5 in 10s) and the unit ends up failed. Without this
# drop-in the box comes back from a reboot with no relay.
mkdir -p /etc/systemd/system/coturn.service.d
printf '[Unit]\nAfter=network-online.target\nWants=network-online.target\n[Service]\nRestartSec=5\n' \
  >/etc/systemd/system/coturn.service.d/override.conf
systemctl daemon-reload
```

The service is started in section 4, after the firewall is up.

## 4. Firewall, then start

Open exactly these. The relay range must match `min-port`/`max-port` in the config.

| Port | Protocol | Why |
|---|---|---|
| 22 | TCP | ssh (restrict to your own address if the provider allows) |
| 3478 | TCP + UDP | STUN/TURN |
| 5349 | TCP + UDP | TURN over TLS / DTLS |
| 49160-49360 | UDP | relay range (**keep in sync with the config**) |
| 80 | TCP | certbot standalone renewals |
| *signalling port* | TCP | once the Rust server exists (put it behind TLS) |

**Do not open** 9641 (Prometheus — not even compiled into Ubuntu's build) or 5766 (admin CLI).

```bash
ufw allow 22/tcp
ufw allow 80/tcp
ufw allow 3478/tcp
ufw allow 3478/udp
ufw allow 5349/tcp
ufw allow 5349/udp
ufw allow 49160:49360/udp
# --force: ufw(8) otherwise prompts, and under ssh the next pasted line gets
# eaten as the answer — the firewall silently stays off
ufw --force enable
ufw status verbose   # expect: Status: active / Default: deny (incoming), allow (outgoing)
```

22 and 80 are already open in the provider's cloud firewall from section 2; add the rest there too — a host
firewall alone will not help if the cloud one drops the relay range, and vice versa.

Now unmask and start:

```bash
systemctl unmask coturn
systemctl enable --now coturn
systemctl status coturn --no-pager
ss -lntup | grep -E '3478|5349'                                       # expect: listening
ss -lntp  | grep 5766                                                 # expect: no output
journalctl -u coturn -n 50 --no-pager | grep -i 'bad configuration'   # expect: no output
```

## 5. Verify the relay actually relays

`turnutils_uclient` and `turnutils_peer` ship in the **`coturn`** package — there is no client-only package,
and installing it starts a second TURN server on the test host. Shut that down first:

```bash
apt install -y coturn
systemctl disable --now coturn
systemctl is-active coturn   # expect: inactive
```

On a host with a **public IPv4** (the config's `denied-peer-ip` block denies every RFC1918 and link-local
range, so a NATed laptop will not work as the peer), start the echo peer:

```bash
turnutils_peer -p 3480
```

Then from a second machine — TLS over TCP to the server, default UDP relay transport, which is what the app
uses. Read the secret from the file rather than pasting it:

```bash
turnutils_uclient -S -t -p 5349 -W "$(cat /etc/aircast/turn.secret)" -u spike \
                  -e <PEER_PUBLIC_IP> -r 3480 -n 10 turn.example.com
```

Drop `-t` to exercise DTLS on the same port instead. `-W` puts the client in REST mode: it derives
`<now+86400>:spike` itself and HMACs it with the secret, so a pass proves the secret and the REST scheme agree.

**Do NOT use `-T`**: it selects an RFC 6062 TCP relay — a path WebRTC never takes, and one the config now
disables with `no-tcp-relay` — implies `-y` so `turnutils_peer` is bypassed entirely, and silently ignores
`-e`/`-r`.

This is the only check here that moves bytes through a port in **49160-49360**. Watch it from the server:

```bash
journalctl -u coturn -f
ss -lnup | awk '$5 ~ /:(49[12][0-9][0-9]|493[0-6][0-9])$/'
```

Second check, closer to the app: open
[webrtc.github.io/samples/src/content/peerconnection/trickle-ice/](https://webrtc.github.io/samples/src/content/peerconnection/trickle-ice/),
add `turns:turn.example.com:5349?transport=tcp` with a hand-minted credential, and confirm a **`relay`**
candidate appears. Mint it in-process, again to keep the secret out of argv:

```bash
python3 - <<'EOF'
import base64, hmac, hashlib, time, pathlib
s = pathlib.Path('/etc/aircast/turn.secret').read_text().strip().encode()
u = f"{int(time.time())+3600}:spike"
print("username:", u)
print("credential:", base64.b64encode(hmac.new(s, u.encode(), hashlib.sha1).digest()).decode())
EOF
```

A `relay` candidate proves a TLS-authenticated Allocate on 5349 — **and only that**. Trickle ICE gathers
candidates and never sends media, so it exercises nothing in 49160-49360: it will show a healthy `relay` row
with that whole range blocked at the cloud firewall. The `turnutils_uclient` run above is what covers it.

When #13's spike runs, set `iceTransportPolicy: 'relay'` on both peers so a working `srflx` path cannot mask a
dead relay range — which is what the app does in production anyway.

**Prove the reboot**, because the drop-in in section 3 is the only thing standing between a power cycle and a
dead relay:

```bash
reboot
# wait, reconnect
systemctl is-active coturn                 # expect: active
ss -lntupn | grep -E ':(3478|5349)\b'      # expect: listening
```

**Worth doing once, because no provider documents it:** leave a sustained UDP relay running for ten minutes
and watch for throttling. #5 found that no provider's own docs say whether their DDoS mitigation rate-limits a
legitimate long-lived UDP flow, and Vultr's DDoS protection additionally requires using Vultr's own resolvers.

## 6. Signalling server

`ops/aircast-signal.service` expects the binary at `/usr/local/bin/aircast-signal` and reads the shared secret
from an environment file. Build that file from the secret file — `printf` is a shell builtin so it forks
nothing, and `cat` takes a path in argv, not the secret:

```bash
install -D -o root -g aircast -m 640 /dev/null /etc/aircast/signal.env
{ printf 'AIRCAST_TURN_SECRET='; cat /etc/aircast/turn.secret; } >/etc/aircast/signal.env
stat -c '%U %G %a' /etc/aircast/signal.env   # expect: root aircast 640
```

`install -D` creates `/etc/aircast` if section 3 has not already; note the parents get default attributes
(0755 root:root), not the file's — which is fine, since the 0640 file is what protects the secret.

The unit is sandboxed (`ProtectSystem=strict`, restricted address families, no new privileges). On first
start, check the sandbox before trusting it:

```bash
systemd-analyze security aircast-signal.service
journalctl -u aircast-signal | grep -iE 'address family|EAFNOSUPPORT|resolve|permission denied'
```

If the binary does its own DNS through glibc and resolution fails, add `AF_NETLINK` to
`RestrictAddressFamilies` — `getaddrinfo` uses NETLINK_ROUTE to enumerate local source addresses. Add it on
evidence from that check, not pre-emptively.

The server itself now exists (`server/aircast_signal.py`) and is Python, not the Rust binary #7 assumed —
recorded here because the unit file's `ExecStart` is unchanged either way: install a two-line shim at
`/usr/local/bin/aircast-signal` that execs the venv interpreter. Full steps in `server/README.md`.

It needs two more variables in the same env file, and they are not secrets — only the first line is:

```bash
{ printf 'AIRCAST_TURN_URLS=turn:<TURN_DOMAIN>:3478?transport=udp,turns:<TURN_DOMAIN>:5349?transport=tcp
'
  printf 'AIRCAST_PORT=8443
'; } >>/etc/aircast/signal.env
```

TLS is terminated by nginx, not by the server — it binds `127.0.0.1:8443` and speaks plain `ws://`, so
nothing else needs the certificate's private key. Install `ops/nginx-aircast-signal.conf.template` with
`SIGNAL_DOMAIN` replaced, and open 443/tcp in the firewall alongside section 4's rules.

**The proxy must set `X-Forwarded-For`** (the template does). The server throttles code-guessing by client IP
and trusts that header only from loopback; without it every client shares one bucket and the first ten misses
lock out everyone.

Record the choice in #16, which owns deployment.

## Provisioned facts

Fill this in and mirror it into #14 — #13's spike reads from here.

```
provider / region:
VPS IPv4:
domain:
coturn version:       (turnserver --version — picks the README that governs the config)
TURN URLs:            turn:<domain>:3478   turns:<domain>:5349
relay range:          49160-49360/udp        (must match /etc/turnserver.conf)
abuse ceilings:       max-bps=1000000  bps-capacity=50000000  total-quota=100  user-quota=20
shared secret:        /etc/aircast/turn.secret (0640 root:aircast) + password manager entry
                      — never pasted into a command, never in this repo
signalling URL:       wss://<domain>:<port>/   (once deployed)
cert expiry / renewal owner:
maintenance window for cert-renewal restarts:
reboot test passed:   yes / no   (section 5)
```
