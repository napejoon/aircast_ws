# Self-hosted signaling + TURN: options, ops and cost

Research asset for [#5](https://github.com/napejoon/aircast_ws/issues/5) (map [#1](https://github.com/napejoon/aircast_ws/issues/1)).
Date: 2026-09-10. Method: primary sources only — coturn's own README/config docs and issue tracker, the RFCs,
and each hosting provider's own pricing/region pages. Prices were read on 2026-09-10 and are quoted as the
page presented them; several provider pages are JS-gated and would not yield a number to a text fetch, which
is stated inline rather than filled in from memory.

Carried in from [#2](https://github.com/napejoon/aircast_ws/issues/2) and
[#3](https://github.com/napejoon/aircast_ws/issues/3): WebRTC with ICE relay **forced** through our own TURN;
H.264 preferred with VP8 as mandatory fallback; receiver is a GStreamer app whose `webrtcbin` takes
`turn-server` and `ice-transport-policy` as plain properties; LiveKit was considered and set aside because it
drags an SFU and a proprietary signalling protocol into a 1:1 topology.

## 1. Decision

**Self-host coturn + our own minimal signalling server on a single Singapore VPS. Shortlist Vultr Singapore
(2 TB pooled allowance, $0.01/GB overage) and Hetzner Singapore — with the warning in §4 that Hetzner's
Singapore allowance is nothing like its European one. Keep Cloudflare Realtime as the documented swap-later
escape hatch, because at 1,000 GB/month free and $0.05/GB after, it is the only managed option in the same
cost universe as self-hosting.**

Region is the hard constraint, not price. Scaleway is eliminated outright: its own availability page lists
Paris, Amsterdam and Warsaw only, and a European relay for a Bangkok sender/receiver pair adds RTT that no
amount of jitter-buffer tuning recovers.

## 2. What coturn actually costs us to run

Ports, from coturn's own `README.turnserver`:

| Option | What coturn documents |
|---|---|
| `listening-port` | "TURN listener port for UDP and TCP listeners (Default: 3478)." |
| `tls-listening-port` | "TURN listener port for TLS and DTLS listeners (Default: 5349)." |
| `min-port` / `max-port` | relay range, "Default value is 49152" / "Default value is 65535, according to RFC 5766" |
| `external-ip` | "TURN Server public/private address mapping, if the server is behind NAT… returned in XOR-RELAYED-ADDRESS field" |
| `listening-ip` | "If no IP(s) specified, then all IPv4 and IPv6 system IPs will be used for listening." |

The canonical Docker mapping in coturn's own README is `-p 3478:3478 -p 3478:3478/udp -p 5349:5349
-p 5349:5349/udp -p 49152-65535:49152-65535/udp`. Narrowing the relay range is expressible on every provider
we looked at, but **coturn's docs state no per-session relay-port count and give no guidance on how narrow is
safe** — that is UNSOURCED, and narrowing it is an operational bet, not a documented recipe.

**The recurring chore that nobody mentions until it bites: coturn has no certificate reload.** `--cert` /
`--pkey` are read at process start, and the project's own issue tracker carries this as an acknowledged, still
open gap — #725 "Let SIGHUP reload OpenSSL certificates" and #1446 "How to reload coturn when ssl
certificates are renewed?". So every renewal is a **restart, which drops live TURN allocations**, i.e. it cuts
any session in flight. Plan the renewal window, or plan to lose a mirror mid-session every 60-90 days.
Nothing in coturn's own docs mentions Let's Encrypt at all (UNSOURCED — that pairing is folklore, however
universal), nor the key-file permissions needed after `--proc-user`/`--proc-group` drop privileges.

Coturn's README does claim TLS 1.3 with ECDHE, DTLS 1.0/1.2, and implementation of RFC 8656 (TURN), 6062 (TCP
relaying), 7350 (DTLS as transport), 8489/5389 (STUN), 5780 and 7635.

**Hardening flags to set before this is publicly reachable**, quoted from the same doc: `denied-peer-ip`
("Options to ban or allow specific ip addresses or ranges of ip addresses"), `no-multicast-peers` ("Disallow
peers on well-known broadcast addresses"), and note that loopback relaying is **denied by default** — the flag
is the opt-in `allow-loopback-peers`, whose own doc says "Allow it only for testing in a development
environment!". The admin CLI is off by default ("Turn ON the CLI support. By default it is always OFF") and
`cli-password` "Default is empty (no password)" with a recommendation to use the encrypted form. Prometheus
metrics exist and are opt-in: `--prometheus` "Would listen on port 9641 under the path /metrics".

## 3. Authentication for an app with no accounts

Three mechanisms, from coturn's own docs:

- `--lt-cred-mech` + `--user` — long-term credentials with **static** accounts provisioned in advance. Wrong
  shape for a 6-digit-code app with no accounts.
- `--no-auth` — "allow anonymous access… This is default option when no authentication-related options are
  set." An open relay. Never.
- `--use-auth-secret` + `--static-auth-secret` — the TURN REST scheme, and the right answer. coturn documents
  the construction verbatim: "This option uses timestamp as part of combined username: usercombo ->
  'timestamp:username', turn user -> usercombo, turn password -> base64(hmac(input_buffer = usercombo,
  key = shared-secret))".

**So credentials are minted, never stored.** When the signalling server pairs the two peers on a 6-digit code,
it computes `username = "<expiry-unix-timestamp>:<opaque-id>"` and
`password = base64(HMAC-SHA1(username, static-auth-secret))`, and hands the same triplet (TURN URL, username,
password) to both peers. The credential's lifetime *is* the embedded timestamp — set it to roughly the
pairing code's own validity window. Note `--stale-nonce` (default 600 s) is a different thing entirely: it is
the TURN protocol's nonce-refresh interval, not the credential expiry, and conflating the two is an easy bug.

Honest note on standing: the REST scheme is described only in `draft-uberti-behave-turn-rest-00`, an
individual Internet-Draft from July 2013 that never advanced past `-00`, has no RFC number and was never
adopted by a working group. It is nonetheless what coturn implements and what every WebRTC deployment uses.

## 4. Bandwidth: the arithmetic, and where it actually hurts

RFC 8656 states the cost qualitatively and gives no numbers: "it comes at a high cost to the provider of the
TURN server since the server typically needs a high-bandwidth connection to the Internet… it is best to use a
TURN server only when a direct communication path cannot be found." We have chosen that last-resort case as
our default, so the arithmetic below is ours, not the RFC's.

For a 1:1 mirror the relay ingests the stream once and egresses it once, so **egress ≈ bitrate × session
hours**. One Mbps sustained is 0.45 GB/hour. Most VPS providers do not bill ingress (Akamai states this
explicitly: "All inbound network transfer" is free), so only the egress column matters.

Monthly egress, one concurrent session, 30-day month:

| Bitrate | 1 h/day | 4 h/day | 8 h/day |
|---|---|---|---|
| 2 Mbps | 27 GB | 108 GB | 216 GB |
| 4 Mbps | 54 GB | 216 GB | 432 GB |
| 8 Mbps | 108 GB | 432 GB | 864 GB |

Multiply by the number of concurrent sessions. The bitrate itself is #8's decision, so the range is
deliberate — but note what the table says: **this is not a trivial amount of traffic at the top end.** 864 GB
a month clears Vultr's 2 TB comfortably, sits inside Akamai's 1 TB, and **blows through DigitalOcean's 500
GiB and Hetzner Singapore's 0.5 TB floor.** Any claim that "egress is negligible for a small team" only holds
at the bottom-left of that table.

## 5. Hosting, from each provider's own pages

| Provider | Cheapest viable plan | Price | Allowance (their wording) | Overage | SEA region |
|---|---|---|---|---|---|
| **Vultr** | Cloud Compute 1 vCPU/1 GB | $6/mo | "2.00 TB Bandwidth", pooled across the account | "$0.01 per GB", flat worldwide | Singapore, Tokyo, Osaka, Seoul, Mumbai |
| **Hetzner** | CX22 | €3.79/mo (EU) | EU "at least 20 TB"; **Singapore/US "0.5 TB to 5 TB"** by plan | "$8.49 per TB" (non-EU) | Singapore |
| **Linode / Akamai** | Nanode 1 GB | $5/mo | "1 TB" outbound, pooled | $0.005/GB core DCs; $0.01/GB distributed; Jakarta $0.015/GB | Singapore (Jakarta closed to new customers since 2025-01-21) |
| **DigitalOcean** | Basic Droplet 512 MB | $4/mo | "500 GiB" | not stated on the pricing page | Singapore (SGP1) |
| **AWS Lightsail** | Linux, IPv6-only 512 MB | $3.50/mo | "1 TB Transfer" | "$0.09 USD per GB" | Singapore, Tokyo |
| **OVHcloud** | VPS-1 | from US$4.54/mo | "Unlimited traffic", 500 Mbps cap; SEA storefront states 500 GB (VPS-1) / 1 TB / 3 TB quotas | n/a — throughput-capped | Singapore |
| **Contabo** | Cloud VPS 4 | €5.50/mo intro, €4.40 regular | "Unlimited Traffic", fair use, "reserves the right to throttle" | none stated | Singapore |
| **Scaleway** | DEV1-S | ~€6.55/mo | egress "included" | n/a | **none — eliminated** |

Two things to carry forward. First, the pricing model splits into *allowance-plus-overage* (Vultr, Hetzner,
Akamai, Lightsail) versus *unmetered-with-throttling* (OVHcloud, Contabo, Scaleway) — for a sustained UDP
relay, an unmetered plan whose provider "reserves the right to throttle" is a worse guarantee than a metered
one with a known per-GB rate. Second, **the same plan name is a different product in Singapore**: Hetzner's
European 20 TB becomes as little as 0.5 TB there, and its Singapore prices would not render to a text fetch at
all, so that gap needs a live check before committing.

Operational facts worth knowing: DigitalOcean's firewall docs support a port range per rule (so
`49152-65535/udp` is directly expressible), and no provider page we read documents a default UDP block —
they are default-deny allow-lists, which means the range must be opened explicitly. Vultr's DDoS Protection
"adds 10Gbps of mitigation capacity per instance" but **requires using Vultr's own nameservers/resolver**,
which is a real constraint on a self-run box. **Whether any provider's DDoS heuristics rate-limit a sustained
legitimate UDP relay flow is not documented by any of them** — a genuine open risk, testable only by running
it. AWS now charges for public IPv4 ("You pay an hourly rate for each public IPv4 address used by your AWS
account", $0.005/hour, in-use or idle); DigitalOcean currently lists an attached IPv4 at $0.00/hour and
charges only for an unattached reserved IP.

## 6. Managed alternatives, and why one of them is genuinely competitive

| Service | Billing unit | Rate | Free tier |
|---|---|---|---|
| **Cloudflare Realtime** | per GB egress | "$0.05 per GB of data egress" | "1,000 GB before any charges start" |
| Twilio Network Traversal | per GB relayed | $0.40/GB US-EU; **$0.60/GB Asia Pacific incl. Singapore**; $0.80/GB Sydney | STUN only, no TURN allowance |
| Metered.ca | per GB, ingress **and** egress | $0.40 → $0.10/GB by tier (base tier prices did not render) | 500 MB/month |
| Xirsys | per GB relayed | $0.50 → $0.09/GB by plan (page would not fetch; search snippet only) | free plan, hard-capped |
| LiveKit Cloud | **per participant-minute** | Ship $50/mo incl. 5,000 min, then "$0.01 per min" | 1,000 min/month |

The unit matters more than the headline. A per-GB service scales directly with whatever bitrate #8 picks; a
per-minute service does not. And the contrast at relay volumes is stark — take 4 Mbps at 4 h/day (216 GB):

- Vultr $6/mo VPS: **$6**, entirely inside the allowance.
- Cloudflare Realtime: **$0**, inside the 1,000 GB free tier.
- Twilio Asia Pacific at $0.60/GB: **~$130/month**.
- Metered at the $0.40/GB tier: **~$86/month**.

At 8 Mbps × 8 h/day (864 GB) Twilio becomes roughly $518/month against the same $6 VPS. So the real finding is
not "self-hosting is cheaper than managed" — it is that **Cloudflare Realtime is the one managed option whose
economics resemble self-hosting**, and it is therefore the escape hatch to design for, per the map's
"swappable to managed later" requirement. Keeping `webrtcbin`'s `turn-server` and the client ICE config as
plain configuration is all the swappability that requires.

## 7. The signalling server: how small it can correctly be

WebRTC deliberately does not specify signalling — the W3C `webrtc-pc` API assumes an out-of-band channel of
the application's choosing, and RFC 9429 (JSEP) governs only what must be exchanged: an SDP offer, an SDP
answer, and ICE candidates delivered individually ("JSEP implementations always provide candidates to the
application individually, consistent with what is needed for Trickle ICE"), with trickle support itself
negotiated in the SDP.

The minimum correct server for this app:

1. Pair two peers on the 6-digit code.
2. **Buffer the offer** until the second peer joins — the answerer cannot answer before it has the offer, so
   forward-and-forget is a bug, not an optimisation.
3. Relay ICE candidates as opaque blobs, **in both directions**, the moment either side produces one.
4. Mint and hand over the TURN REST credentials (§3) alongside the peer's SDP.
5. Expire the code and drop the buffered offer on connect or on a short TTL.

Code collision and brute-force resistance belong to [#6](https://github.com/napejoon/aircast_ws/issues/6), not
here.

**Reusable implementations, read from their own repos:**

- `flutter-webrtc/flutter-webrtc-server` (Go) — has signalling, a web demo, a pion TURN server and TURN REST
  support. Its own README warns: "if you need to use it in a production environment, you need more testing."
  Good reference, not a dependency.
- GStreamer's `gst-webrtc-signalling-server` (in gst-plugins-rs `net/webrtc`, MPL-2.0) — ships with the
  webrtcsink/webrtcsrc batteries. But it speaks **GStreamer's own JSON envelope** over `gstwebrtc://`, not
  generic SDP-over-WebSocket, so a Flutter peer would have to implement that protocol. Since #3 already ruled
  `webrtcsrc` out on portability grounds (no `gstreamer1.0-plugins-rs` package on Ubuntu), this is a no.
- Pion's `examples/` — demo code, documented as examples rather than a protocol. Reference only.
- coturn ships no signalling component at all, and its docs recommend none.

Given a 6-digit pairing code, a credential mint and two message types, **write it.** It is a few hundred
lines, and every candidate above would need adapting to the pairing flow anyway.

## 8. What #14 needs from this

Concrete provisioning checklist for [#14](https://github.com/napejoon/aircast_ws/issues/14):

- One VPS in **Singapore** (shortlist: Vultr 2 TB, or Hetzner SGP after checking its real allowance and price).
- A domain plus a TLS certificate; note the renewal-restart problem in §2 and decide the maintenance window
  up front.
- Firewall: 3478/tcp+udp, 5349/tcp+udp, the chosen relay range (default 49152-65535/udp — narrow it and
  record the number chosen), the signalling port, and **not** 9641 (Prometheus) or the admin CLI port.
- coturn config: `--use-auth-secret` + `--static-auth-secret`, `--external-ip` if the VPS is NAT'd,
  `--denied-peer-ip` for private ranges, `--no-multicast-peers`, `--proc-user`/`--proc-group`, no
  `--allow-loopback-peers`, CLI left off.
- Record where the shared secret lives, and how the signalling server reads it.

## 9. Reliability of this document

Two independent research passes, one on coturn and the specs, one on hosting prices. **A correction worth
recording: the first pass's bandwidth table was wrong by roughly 16x** (it reported ~13 GB/month for 4 Mbps at
4 h/day). The correct figure is 216 GB/month — 1 Mbps sustained is 0.45 GB/hour, so 4 Mbps × 120 hours is
216 GB. The table in §4 uses the corrected arithmetic, and the conclusion changed with it: egress is *not*
negligible at the top of the range, and it is exactly what disqualifies the 500 GB plans.

Unresolved, and flagged rather than filled in: Hetzner's Singapore plan prices and its EU overage rate (page
is JS-gated), DigitalOcean's overage rate (not stated on its own pricing page), AWS and GCP per-GB egress
tables (interactive tables that would not render), Metered's base tier prices, Xirsys's rates (page would not
fetch; search snippet only), coturn's per-session relay-port count, key-file permissions after privilege
drop, and whether any provider's DDoS mitigation interferes with sustained UDP relaying.
