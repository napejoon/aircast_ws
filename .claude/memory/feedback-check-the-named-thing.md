---
name: feedback-check-the-named-thing
description: "When the user names a visual problem (a colour, an overlap), check that exact thing in a screenshot before reporting it fixed"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 59247566-5c97-4867-850c-23b2c5c46583
  modified: 2026-09-25T13:32:32.639Z
---

When the user complains about a specific visual thing, verify that thing in a screenshot before
saying it is done.

**Why:** asked to "fix the sender colours", I changed the background and card gradients and
reported done; the user came back with "ยังเป็น turquoise อยู่เลย" — the full-width Start button
and the code slots were still the live green `#6FE3C4`, the most prominent thing on the screen.

**How to apply:** after a design change, look at the screenshot for the element the user actually
named (here: anything turquoise), not just for "looks different". Grep the palette constant's uses
(`_live` in `sender/lib/main.dart`) rather than trusting the ones already touched. Reports go in
terse Thai ([[user-thai-terse-reports]]).
