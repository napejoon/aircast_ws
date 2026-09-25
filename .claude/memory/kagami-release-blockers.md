---
name: kagami-release-blockers
description: "Why Kagami releases stay drafts — update key, APK release key, and the code-signing decision (no paid cert)"
metadata:
  node_type: memory
  type: project
  originSessionId: 59247566-5c97-4867-850c-23b2c5c46583
  modified: 2026-09-25T13:32:28.748Z
---

Kagami releases are drafts on purpose (as of 2026-09-25, latest v0.1.11). Three things block publishing:

- **Update signing key** — `AIRCAST_UPDATE_PK` in `receiver/update_check.c` is all zeros. The user
  must generate it with `minisign -G` themselves (install: `winget install jedisct1.minisign`), keep
  the `.key` off GitHub and away from the agent (`docs/threat-model.md:83`), and send only line 2 of
  the `.pub`. They have no offline machine; they chose to skip for now.
- **Release APK key** — release APKs are `*-unsigned.apk` and do not install; the tablet runs CI
  debug APKs (committed debug keystore).
- **Windows code-signing: the user ruled out a paid cert (2026-09-25, "แพงมาก").** Ship the MSI
  unsigned; users click SmartScreen "More info → Run anyway". Free routes mentioned but not verified:
  SignPath Foundation (OSS, repo must be public) and Microsoft Store MSIX (MSIX has no custom
  action, so the registry prebuild would need rethinking). An offer to document the SmartScreen
  click-through in the README was not answered.

**How to apply:** do not re-propose buying a certificate; when release work comes up, the next
step is the user's minisign key. See [[kagami-verify-loop]] for building and installing.
