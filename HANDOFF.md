# Kagami — งานค้าง / open work

**อัปเดตล่าสุด:** 2026-09-25 · **สถานะ:** main อยู่ที่ `3e472ed` = tag `v0.1.11` (draft) ติดตั้งใช้งานจริงแล้วทั้ง Windows (MSI) และ tablet (CI debug APK) ไม่มี branch ค้าง merge

## ทำอะไรอยู่ / What this is

Kagami (鏡 เดิมชื่อ Quoise, identifier เดิม `aircast`) คือโปรแกรมมิเรอร์จอ: receiver เป็น GTK4 + GStreamer
บน Windows (`receiver/main.c`), sender เป็น Flutter บน Android (`sender/lib/`) จับคู่ด้วยรหัส 6 หลัก/QR
ผ่าน WebRTC (มีทาง USB ด้วย)

รอบนี้ไล่ปิดรายการ 1–7 ที่ค้างจากรอบก่อน ระหว่างทดสอบกับ tablet จริงเจอบั๊กใหม่อีก 3 ตัวที่ compiler/CI
จับไม่ได้เลย (การอัดไม่เคยได้ภาพ, shortcut ตายบนแป้นไทย, cast ซ้ำหลังมือถือตาย) — **บทเรียนเดิมยังจริง:
CI เขียวไม่ได้แปลว่าใช้ได้ ต้อง cast จริงแล้วดูผล**

## ทำสำเร็จแล้ว / Done

ทุกข้อทดสอบบน tablet จริงกับ MSI ที่ CI build (แตกด้วย `msiexec /a`) ก่อน merge

- **Fullscreen ออกมาแล้ว strip จมใต้ taskbar (PR #63)** — `gtk_window_maximize` ของ GDK กับ title bar
  แบบ native (`GTK_CSD=0`) ให้ client area สูงเท่า work area แล้วเอา title bar ซ้อนบน: วัดได้ client
  2560x1392 บน work area 1392 → strip 23 px ใต้ taskbar ลอง 5 แบบใน build diag เดียว:
  plain / deferred 500 ms / unmaximize-then-maximize ได้ 1392 หมด, ปล่อย GTK = 1100x760 ไม่ maximize,
  **`ShowWindow(SW_MAXIMIZE)` ได้ 1369 strip ครบ** ← ใช้ตัวนี้
- **cast ซ้ำหลังมือถือตายโดยไม่ส่ง bye (PR #63)** — server ไม่แจ้ง receiver ว่า sender ออก
  (`server/aircast_signal.py` `_leave`) offer ของมือถือตัวใหม่เลยไปลง webrtcbin ของตัวที่ตาย
  → มือถือค้าง ICE checking แล้ว failed ใน 14 วิ แก้ที่ `on_message` "peer": ถ้า webrtcbin มี remote
  description แล้วให้ drop session ก่อน ผล: force-stop กลาง cast แล้ว cast ใหม่ connected ใน 0.2 วิ ค้าง 30 วิ ไม่หลุด
- **Shortcut F/R/D ตายบนแป้นไทย (PR #63)** — layout ของเครื่องนี้คือ Thai (`041E`) กด R ได้ "พ"
  → keyval ไม่ใช่ `GDK_KEY_r` แก้ด้วย `gdk_key_event_matches` (fallback ตามตำแหน่งปุ่ม)
- **การอัดไม่เคยได้ภาพเลย (PR #63)** — tee อยู่ใน tail bin แต่ record branch ถูก add เข้า pipeline
  → `gst_pad_link` ข้าม hierarchy = `GST_PAD_LINK_WRONG_HIERARCHY` ที่ไม่มีใครเช็ค ทุกไฟล์ = header
  336 bytes ไม่มี track แก้: add branch เข้า bin เดียวกับ tee, tail bin ตั้ง `message-forward`
  ให้ EOS ถึง bus, link พังแล้วบอก ผล: อัด 9.0 วิ = 6.7 MB, 466 packets, H.264 2304x1440, ffmpeg ดึงเฟรมได้
  ไฟล์ไปที่ `Videos\kagami-*.mkv`
- **debug keystore ใช้ได้แล้ว (PR #62)** — `scaffold-sender.sh` เขียน `signingConfigs.getByName("debug")`
  ชี้ `sender/android/debug.keystore` ตรงๆ พิสูจน์: `adb install -r` ทับได้, `firstInstallTime` ไม่เปลี่ยน
- **สี sender (PR #62, #64)** — พื้น/การ์ดเป็น gradient เดียวกับ receiver, ปุ่ม Start + ขีดรหัสเปลี่ยนจาก
  เขียว `#6FE3C4` เป็น moonlight `#E8EDF2` เขียวเหลือแค่จุด/ข้อความสถานะตอน cast (ตรงกับ beacon ของ receiver)
  ปุ่มกล้องย้ายไปขวาบน
- **สแกน QR แล้ว mirror เลย (PR #65)** — เฉพาะ QR ที่ชี้ server ที่แอป build มา (`AIRCAST_SIGNAL`
  ปกติ `wss://aircast.cloud/ws`, เทียบ scheme+host+port ด้วย `PairingPayload.isOn`) QR ที่ชี้ host อื่น
  ยังต้องกด Start เอง (กันส่งจอไป relay ของคนแปลกหน้า) ผู้ใช้ลองสแกนจริงแล้ว ผ่าน
- **gtk_init ~0.9–1.3 วิ: สรุปว่าแก้ระดับแอปไม่ได้** — ลอง thread อุ่น GIO registry ก่อน gtk_init:
  0.85/0.91 วิ vs ไม่อุ่น 0.92/0.88 วิ ไม่ต่าง ทิ้งไปแล้ว (รวมกับที่รอบก่อนลองทุก GDK flag แล้วไม่ขยับ)
- **Release:** เหลือ draft ตัวเดียว v0.1.11 (ลบ draft v0.1.6–v0.1.10 แล้ว **tag ยังอยู่ครบ**)
  hash MSI v0.1.11 `7b2286302b371321382b712b5ed4b90a55247159876bba6b40471797ada092d7` ตรงกับ digest ของ GitHub

## เหลืออะไร / What's left

1. **key สำหรับเซ็นอัปเดต (บล็อกที่ผู้ใช้)** — `AIRCAST_UPDATE_PK[32] = { 0 }` ใน `receiver/update_check.c`
   ผู้ใช้ต้องรันเอง: `winget install jedisct1.minisign` → `minisign -G` (ตั้งรหัส) → ย้าย `.key` ลง USB
   → ส่งแค่**บรรทัดที่ 2 ของ `.pub`** มา private key ห้ามเข้า GitHub Actions/agent (`docs/threat-model.md:83`)
   ยังไม่มีเครื่อง offline ผู้ใช้เลือกข้ามไปก่อน ได้ `.pub` แล้วที่เหลือ (ใส่ 32 ไบต์, README, manifest) ทำต่อได้ทันที
2. **ปล่อย release จริง** — รอข้อ 1 + keystore สำหรับเซ็น APK release (ตอนนี้ `*-unsigned.apk` ติดตั้งไม่ได้)
   workflow จงใจจบที่ draft
3. **code-signing Windows: ผู้ใช้ตัดทางซื้อ cert แล้ว ("แพงมาก")** — ปล่อย MSI แบบไม่เซ็น ผู้ใช้เจอ
   SmartScreen ต้องกด More info → Run anyway ทางฟรีที่พูดถึงแต่**ยังไม่ได้ตรวจเงื่อนไข**: SignPath Foundation
   (OSS, repo ต้อง public) และ MSIX ผ่าน Store (MSIX ไม่มี custom action → registry prebuild ต้องคิดใหม่)
   เสนอเขียนวิธีกดผ่าน SmartScreen ลง README ไว้ ผู้ใช้ยังไม่ตอบ
4. **ข้อจำกัดที่รู้แล้ว (ไม่ใช่บั๊กที่ต้องรีบ)**
   - มือถือตายระหว่าง**กำลังอัด**แล้ว cast ใหม่ภายใน ~700 ms: offer ลง webrtcbin เก่าที่รอปิดไฟล์อยู่
     → cast นั้นล้มหนึ่งครั้ง กดใหม่ได้ (มี comment `ponytail:` ใน `on_message` "peer" บอกวิธีแก้)
   - จอมือถือนิ่ง = MediaProjection ไม่ส่งเฟรม → ไฟล์อัดมีแค่ keyframe เดียว เป็นพฤติกรรมปกติ
   - "Negotiating…" ค้างบน toolbar ที่เคยเห็น น่าจะเป็นบั๊ก cast ซ้ำตัวเดียวกัน **ยังไม่ได้ยืนยันแยก**
   - debug APK จาก CI มี `versionName=0.1.0` เสมอ (ไม่รับเลข tag)
5. **branch `diag/fullscreen-geometry` บน remote** — build diag (knob `KAGAMI_FS_RESTORE`, thread อุ่น GIO)
   ถูกแทนด้วย PR #63 แล้ว ลบได้
6. **macOS / iOS ยังไม่เริ่ม** — ดูบันทึกรอบก่อนใน git history ของไฟล์นี้ (`git log -p HANDOFF.md`)

## วิธีเริ่ม / How to pick it up

```bash
git log --oneline -1 main              # 3e472ed
gh release list --limit 3              # v0.1.11 Draft
gh run list --branch <branch> --limit 1
```

ทดสอบ receiver โดยไม่ต้องติดตั้ง (ไม่ต้อง UAC):

```bash
gh run download <run-id> -n kagami-msi -D fixX          # รัน background + วน retry เน็ตที่นี่หลุดบ่อย
cd fixX && MSYS2_ARG_CONV_EXCL='*' msiexec /a "<abs>\\Kagami-0.0.0-x64.msi" /qn TARGETDIR="<abs>\\ext"
env -u HOME ext/PFiles64/Kagami/bin/kagami.exe --prebuild-registry   # ครั้งแรกช้า อย่าคิดว่าค้าง
env -u HOME GST_DEBUG=webrtcbin:4 ext/PFiles64/Kagami/bin/kagami.exe --code 424242 > run.log 2>&1 &
```

**ชุดสคริปต์ทดสอบกับ tablet อยู่ใน `%TEMP%\kagami-work\` — ไม่ได้อยู่ใน git** (`cast.sh` สั่ง tablet
พิมพ์รหัส 424242 + กดยอมแชร์จอ, `key3.ps1` ส่งปุ่มด้วย PostMessage+scan code, `key.ps1` F11,
`cap.ps1` จับภาพด้วย PrintWindow, `verify63c.sh` เทสต์ครบชุด recast/อัด/fullscreen) ถ้าหายต้องเขียนใหม่

**กับดัก — อ่านก่อนลงมือ** (ฉบับเต็มใน `.claude/memory/kagami-verify-loop.md`)

- **sender ที่ค้าง cast จากเทสต์ก่อนจะต่อเข้า receiver ตัวใหม่เอง** แล้ว `cast.sh` ไป force-stop มัน
  ดูเหมือน cast หลุด — force-stop sender ก่อนเริ่มทุกรอบ (รอบนี้เสียเวลาไล่ "cast หลุด" ไปหลายรอบเพราะข้อนี้
  แต่มันก็พาไปเจอบั๊กจริงของ recast)
- **แป้นพิมพ์เครื่องนี้เป็นไทย** — `keybd_event`/SetForegroundWindow ส่งตัวอักษรไม่ถึง GTK ใช้ `key3.ps1`
- **จอ tablet นิ่ง = อัดได้เฟรมเดียว** เทสต์อัดต้องขยับจอ: `adb shell cmd statusbar expand-notifications` / `collapse`
  แล้วเช็คด้วย `ffprobe -show_packets`
- **รันจาก Git Bash ต้อง `env -u HOME`** ไม่งั้น `HOME=/c/Users/...` หลุดเข้า GLib
- **msiexec ติดตั้งจริงใช้ `/qb` ห้าม `/qn`** (perMachine ต้อง UAC) และต้อง `MSYS2_ARG_CONV_EXCL='*'`
- **merge ต้องรอ CI ของ PR จบ** (`BLOCKED`) และ**เช็คว่า MERGED ก่อน tag เสมอ**; `--delete-branch`
  อาจล้มตอนเน็ตหลุดทั้งที่ merge สำเร็จ — เช็ค `git ls-remote --heads` แล้วลบเอง
- **adb** อยู่ที่ `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Google.PlatformTools_*\platform-tools\adb.exe`
- APK ของ CI ลงทับได้ด้วย `adb install -r` แล้ว; APK ใน release ยังไม่เซ็น ลงไม่ได้
