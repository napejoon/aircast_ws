---
name: user-thai-terse-reports
description: "User (napejoon) writes terse Thai bug reports; expects root-cause fixes on the current branch, reply in Thai"
metadata: 
  node_type: memory
  type: user
  originSessionId: 0feef42a-c123-4f5c-9d72-3ec4d57c21f3
  modified: 2026-09-13T07:33:51.597Z
---

User writes short Thai symptom reports ("ล่าสุดคือต่อได้ แต่ซักพักก็ค้างไป") with no logs or timings, and expects the assistant to trace the cause and fix it on the current fix/* branch. Test hardware: Samsung Tab S10 FE on Android 16 (phone side), Windows desktop receiver, Singapore VPS relay.

**Why:** Session history on aircast_ws is a chain of symptom → fix commits; the user does not narrate steps.
**How to apply:** Reply in Thai. Rank causes with the time-to-freeze and the tell-tale sign on each side so the user can match without being asked. Ask for logs only after delivering the fix. See [[aircast-turn-ttl-bug]].
