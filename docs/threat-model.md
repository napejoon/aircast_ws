# Threat model: the update channel

Written before the feature was built, because the feature is the one place this
program takes instructions from the internet about something the user might then
run. It exists so the notify-only design never implies safety it does not have.

## The decision

**The app never downloads and never executes anything.** It fetches one small
JSON manifest that the maintainer signed on an offline machine, verifies an
Ed25519 signature over it against a public key compiled into the binary, and
uses the result for exactly two things: a number to compare, and a boolean that
shows a notice. Clicking the notice opens a GitHub tag URL — built from the
verified integers, never a string from the network — in the user's browser.

Self-updating is not built, and the reason is not effort. With no code-signing
certificate, an in-app download-and-execute path is a permanently armed remote
code execution channel whose cryptographic half cannot be completed, and it
would strip the Mark of the Web and SmartScreen evaluation the browser performs
for free.

The signature sits on top of notify-only because a detached signature's trust
root is the maintainer's offline key, **not** GitHub. It does not authenticate
the installer the user eventually runs — nothing here does. It authenticates the
*decision to nudge*, which is what removes this program from the kill chain of
an attacker who has taken over the repository.

## Threats

| | Verdict | Mechanism |
|---|---|---|
| **T1** Network attacker (MITM, hostile Wi-Fi, DNS) | Partly | libsoup fails closed on a bad certificate; the URL is a compile-time `https://` literal and the final URI's scheme is re-checked after redirects, because libsoup will follow https → http. Body caps of 8 KB and 4 KB, a 10 s per-I/O timeout and a 20 s wall-clock cancel. Above all the transport is **not load-bearing**: the signature is end-to-end from an offline key, so a full MITM can suppress a notice but cannot forge one. |
| **T2** Compromised GitHub account, Actions token, or malicious workflow | Partly | The signing key never enters Actions in any form. An attacker with full repo write can publish a release and edit its notes, but cannot produce a manifest this app accepts — so the app will not nudge anyone toward it. **Not covered:** a malicious pull request the maintainer reviews, merges and then signs. |
| **T3** Tampered or swapped release asset | Partly, and the weak half is the half users touch | The signed manifest carries the expected filename and SHA-256, and the app displays both. **No code ever hashes the installer** — the user downloads it in a browser. Full coverage needs a code-signing certificate. |
| **T4** Downgrade, rollback, replay of an old signed manifest | Covered for the nudge | The floor is `max(compiled-in version, persisted high-water mark)`, and only a verified manifest raises the mark. GitHub's own choice of "latest" is therefore not load-bearing: a `make_latest` flip or a `prerelease` flag can only produce a refusal, which the app reports rather than swallowing. |
| **T5** Local attacker on the same machine | Partly | No download means no staging directory, no TOCTOU window and no installer launched from a path we chose. `harden_environment()` clears the GIO and GStreamer module-path variables as the first statement of `main()` — on Windows GLib does not guard them, so anything that can write `HKCU\Environment` otherwise gets arbitrary DLLs loaded into this process. The per-machine install into `%ProgramFiles%` is what makes the application directory, which the loader searches first, not user-writable. **Not covered:** code already running as the user. |
| **T6** The updater as an RCE channel | Removed by construction | No download, no write of network-supplied bytes, no process creation. The honest qualification: the notice still opens a URI, so a network attacker who can only lie about a number can still cause the user's own browser to launch, at a URL they cannot influence. |

## What the app does, in order

1. **Startup, before any network.** `harden_environment()`. No signing key
   compiled in means the check never runs and the UI says so — a check that
   looks healthy while verifying nothing is the failure this design exists to
   prevent.
2. **Throttle**, keyed on *attempt* time, persisted in
   `<config>/aircast/update.ini`: at most one automatic attempt a day. The
   button is exempt; a hostile network cannot spin either.
3. **Fetch** the manifest and its `.minisig`, under the caps and timeouts above.
4. **Verify before parsing anything.** Unverified bytes never reach the JSON
   parser, so a parser bug is not reachable by anyone without the key.
5. **Check, in order:** `schema == 1`; `product` equals `aircast-receiver`
   exactly; `version` parses to exactly three integers, each ≤ 999; version is
   above the floor; `asset` and `sha256` match a character class.
6. **Render from the parsed values only**, with `gtk_label_set_text` — never
   `set_markup`, never a sentence the feed wrote.
7. **The button** opens `…/releases/tag/vX.Y.Z`, built from the three verified
   integers.

## Failure behaviour

Nothing produces a modal, a nag, or a "proceed anyway". There is one passive
status line. Past 30 days without a successful check it says so and points at
the releases page — that line is what turns an indefinite freeze from invisible
into legible, and it is the cheapest control here.

A signed-but-invalid manifest does **not** count as a successful check. A
correctly-signed *older* manifest being replayed is reported explicitly
("refused an older update manifest"), because a `make_latest` flip, a
`prerelease` flag and a deleted asset all make the check *succeed* with stale
truth, where last-check-time alone would show nothing.

## Cryptography

Ed25519 over BLAKE2b-512, via libsodium (`crypto_sign_verify_detached`,
`crypto_generichash`). Windows CNG is not an option: `bcrypt.h` defines
Curve25519 as an ECDH curve and has no EdDSA algorithm identifier.

The signed bytes are the complete published `aircast-update.json`. The minisign
container's untrusted and trusted comments are **never read**, which is why its
second "global" signature is not checked: nothing it covers is used. Making that
container the trust boundary is the usual way to get minisign wrong.

Key generation, once, on an offline machine:

```
minisign -G -p aircast-update.pub -s aircast-update.key
```

The private half lives there plus two encrypted offline backups, and nowhere
else — never a GitHub Actions secret, not even behind an environment approval: a
solo maintainer is an admin and can bypass protection rules, and once approved
the key is plaintext in the runner. The public half's 32 bytes go into
`receiver/update_check.c` as `AIRCAST_UPDATE_PK`, and the full `.pub` line is
printed in `receiver/README.md` so a human can cross-check it.

**There is no key rotation, deliberately.** Dual-key OR gives an attacker a
choice of keys and no revocation; Sparkle's staged AND needs a second trust
anchor this project does not have. Single key, and the consequences are
survivable only because the key's authority stops at "show a notice": a lost key
means notices stop, a stolen key buys a false notice pointing at a GitHub tag the
thief does not control.

**The sender's APK is signed by a second, unrelated key**, an Android keystore
made once on the same offline machine:

```
keytool -genkeypair -v -keystore aircast-sender.jks -alias aircast \
  -keyalg RSA -keysize 4096 -validity 10000
```

Losing this one costs more than losing the minisign key. That key's authority
stops at "show a notice"; this key *is* the app's identity. Android will only
accept an update signed by the same key, so a lost keystore means every phone
that has aircast must uninstall it by hand before it can take another release —
and a stolen keystore lets the thief replace aircast on any of those phones,
with no notice and no dialog. Same storage rule as the minisign key, for a
bigger reason: the offline machine and two encrypted backups, never an Actions
secret. `-validity 10000` because an expired signing key cannot be rotated
either.

## Release ceremony

On the maintainer's machine, about five minutes. The steps that are easy to skip
and expensive to skip are 1, 4, 5 and 10.

1. **Note the commit SHA from your own clone** of the tag you are about to push.
   *Skipped:* nothing binds the artifact to source you actually read.
2. **Push the tag.** CI builds, attests, drafts, and runs `--selftest`.
3. **Download the draft's assets** (`gh release download vX.Y.Z`) — the MSI and
   the unsigned APK.
   *Skipped:* you end up signing a digest CI handed you, which gives a
   compromised Actions token your offline key's authority over its own build.
4. **Gate on the attestation, with the ref pinned:**
   ```
   gh attestation verify <file> -R napejoon/aircast_ws \
     --cert-identity 'https://github.com/napejoon/aircast_ws/.github/workflows/release.yml@refs/tags/vX.Y.Z' \
     --cert-oidc-issuer https://token.actions.githubusercontent.com \
     --source-digest <SHA from step 1> --deny-self-hosted-runners
   ```
   *Skipped, or run with `--signer-workflow` instead:* that flag matches the
   workflow path only, so an attacker pushes a branch, edits `release.yml`
   there, runs it, and the identity becomes `…@refs/heads/evil` — which passes.
   The pinned `--cert-identity` is the whole check. Run it once per file: the
   MSI and the APK are separate subjects of the same attestation. For the APK
   this is the only moment provenance is checkable at all — apksigner in step 9
   changes the file's digest, so the attestation stops matching and can never be
   re-run. *Skipped for the APK:* you sign whatever the draft happened to hold.
5. **Hash it yourself:** `certutil -hashfile <msi> SHA256`.
6. **Write `aircast-update.json` by hand.** Five fields:
   ```json
   {"schema":1,"product":"aircast-receiver","version":"1.4.0","asset":"Aircast-Receiver-1.4.0-x64.msi","sha256":"<64 hex>"}
   ```
7. **Sign it, and confirm the algorithm tag:**
   ```
   minisign -S -H -s aircast-update.key -m aircast-update.json
   head -2 aircast-update.json.minisig | tail -1 | base64 -d | head -c2   # must print: ED
   ```
   *Skipped:* a legacy `Ed` signature is refused by every shipped copy, and you
   find out when users report that updates stopped.
8. **Run the shipped verifier against what you just produced:**
   `aircast-receiver --verify-manifest aircast-update.json --verify-signature aircast-update.json.minisig`
   — exit 0 required. *Skipped:* you ship a release every installed copy
   silently refuses, which is indistinguishable from a freeze attack.
9. **Sign the APK, then upload everything and publish.** zipalign first,
   apksigner second, and nothing touches the zip afterwards — apksigner's
   signature covers the file layout, so aligning a signed APK invalidates it:
   ```
   zipalign -P 16 -f 4 Aircast-Sender-X.Y.Z-unsigned.apk Aircast-Sender-X.Y.Z.apk
   apksigner sign --ks aircast-sender.jks --ks-key-alias aircast Aircast-Sender-X.Y.Z.apk
   apksigner verify --print-certs --verbose Aircast-Sender-X.Y.Z.apk
   ```
   `-P 16` is for the 16 KB-page devices; the APK carries uncompressed `.so`
   files from libwebrtc and the Flutter engine. Pass no `--v1/--v2/--v3` and no
   `--min-sdk-version`: apksigner reads `minSdk` out of the APK and picks the
   schemes from it, which at 26 is v2 and v3 — pass one by hand and you are
   overriding the manifest with a guess. The verify must print `v2 …: true` and
   a certificate fingerprint you recognise.
   Then upload `aircast-update.json`, its `.minisig` and the signed APK, remove
   the unsigned one (`gh release delete-asset vX.Y.Z Aircast-Sender-X.Y.Z-unsigned.apk`)
   so nobody downloads a file that cannot install, and publish
   (`gh release edit vX.Y.Z --draft=false`).
   Publishing is what makes the assets immutable; the draft window is mutable by
   design, so keep it short.
10. **Re-download the published assets and re-hash both.**
    `certutil -hashfile <msi> SHA256`, and the same for the APK, against the
    local copies you signed. If either differs, delete the release. `apksigner
    verify` is not the check here: it proves the file is signed by some key, not
    that it is the file you uploaded. *Skipped:* an asset swapped during the draft
    window ships with a manifest that authentically vouches for a hash nobody
    will ever compare it to — no code checks it, so this manual step is the only
    thing that catches it.
11. **Bump `AIRCAST_VERSION`** for the next cycle.

If this ceremony is not being performed reliably, the correct response is to
stop signing and remove the update check — not to move the key into CI.

## Accepted residual risk

- **The installer the user runs is authenticated by nothing.** The app never
  hashes it. The SHA-256 shown is correct, signed, and will be checked by almost
  nobody. A genuine and a tampered installer produce the identical "Windows
  protected your PC" dialog. **This is the largest hole, and only a code-signing
  certificate closes it.**
- **Every release trains the user to click through SmartScreen**, which is
  exactly the reflex an attacker needs. Inherent to shipping unsigned.
- **A reviewed-and-merged malicious PR is not covered.** Signing proves who
  signed, never that what was signed is benign.
- **Release notes stay editable by anyone with repo write**, even on an
  immutable release. The app shows the expected filename and hash so the page's
  prose cannot quietly redirect the user, but "hotfix, install this instead" on
  the page we sent them to is still available to that attacker.
- **Freeze is detectable, not preventable.** Anyone who can drop packets parks a
  user on an old build; the 30-day line is observability, not a fix.
- **A local root-CA anchor MITMs the check.** Accepted: the signature makes the
  consequence a suppressed notice.
- **Code running as the user owns everything.** It can patch the 32 public-key
  bytes out of an unsigned `.exe` whose value is public in this repository, or
  rewrite the high-water mark to suppress notices forever. The per-machine
  install raises the bar; nothing removes it.
- **No revocation.** A stolen key is valid against every installed copy forever;
  recovery is an out-of-band announcement.
- **`--insecure` exists.** A user who passes it gets a plaintext signalling
  channel and everything that follows from it.

## If the self-update path is ever built

All of this, before a single byte is downloaded: a code-signing certificate
whose key never enters Actions; a freshly named random directory under
LocalAppData whose resolved path and owner SID are verified; one handle held
from first byte to consumer read; an explicit environment block for the child
process; an MSI with zero binary custom actions and an upgrade table that
refuses downgrades; `/l*v` logging, because `/passive` shows no errors; a written
Zone.Identifier; and an answer for what happens when the app never comes back.
That list is the price, and it is why this design does not pay it.
