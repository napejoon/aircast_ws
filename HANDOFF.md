# Kagami — งานค้าง / open work

**อัปเดตล่าสุด:** 2026-09-24 · **สถานะ:** main อยู่ที่ `9d1f6a3` (v0.1.8 ติดตั้งใช้งานจริงแล้วทั้ง Windows และ Android) มี 2 branch ที่ CI เขียวแต่ยังไม่ merge

## ทำอะไรอยู่ / What this is

Kagami คือโปรแกรมมิเรอร์จอ: receiver เป็น GTK4 + GStreamer บน Windows, sender เป็น Flutter บน Android
จับคู่กันด้วยรหัส 6 หลักผ่าน WebRTC (มีทาง USB ด้วย)

เดิมชื่อ Quoise และใช้ identifier ว่า `aircast` — รอบนี้เปลี่ยนเป็น Kagami (鏡 = กระจก) ทั้งชื่อแสดงผล
และ identifier พร้อมทำ logo ครั้งแรก เปลี่ยน palette ทั้งสองฝั่ง แล้วไล่แก้บั๊กที่โผล่ตามมา

**บทเรียนหลักของรอบนี้:** บั๊กทุกตัวที่เจอ compiler จับไม่ได้สักตัว — เจอจากการดูภาพหน้าจอ
จากการ cast จริง และจาก code review ที่อ่านโค้ดเทียบกับสิ่งที่ comment อ้างไว้ อย่าเชื่อว่า CI เขียวแล้วของใช้ได้

## ทำสำเร็จแล้ว / Done

**Startup: 22.83 → 1.48 วินาที (warm), 7.04 → 2.06 วินาที (cold หลังติดตั้ง)** วัดจาก mark ในโปรแกรมเอง
- prune GStreamer plugin 299 → 18 ไฟล์ (PR #51) — `gst_init` cold 21.28 → 4.82 วิ
- prebuild plugin registry ตอนติดตั้งด้วย MSI custom action (PR #53) — cold เหลือ 0.03 วิ
  ราคาย้ายไปอยู่ที่ตอนติดตั้ง (log ของ CA เอง: `gst_init done at 3.20 s`)
- MSI 124 → 70 MB

**Rebrand เป็น Kagami (PR #55)** ชื่อแสดงผล + identifier ทั้งหมด: `kagami.exe`, `io.kagami.receiver`,
`kagami://pair`, update manifest product `kagami`, Flutter org `io.kagami`
- **ไม่แตะ** 3 อย่างเพราะไม่ใช่ชื่อโปรแกรม: โดเมน `aircast.cloud` (deploy อยู่จริง),
  `aircast-update.json` (release asset ที่ปล่อยไปแล้ว), prefix `aircast_` ของฟังก์ชัน C (internal API)
- logo: วงกลมผ่าครึ่ง (แสงจันทร์ / แสงสะท้อน) — `branding/make-icons.py` อธิบาย mark ครั้งเดียว
  แล้วเรนเดอร์ออก `.ico` (exe resource + Apps and Features), hicolor PNG (GTK), Android mipmap 5 density

**Palette "moon over deep water"** ทั้ง receiver และ sender:
`#e8edf2` แสงจันทร์ (รหัส) · `#6fe3c4` เขียวทะเล (live เท่านั้น ไม่ใช้กับตัวอักษร) · `#ffb35c` อำพัน (recording)

**บั๊กที่แก้แล้วและพิสูจน์ด้วยการใช้งานจริง (PR #56)**
- layout loop: `GtkAspectFrame` obey_child อ่าน ratio จาก paintable ที่ประกาศขนาดเอง → วนไม่จบ
  GTK ยอมแพ้ (`layout continuously requested, giving up after 4 tries`) ทิ้ง allocation ผิดไว้ strip โดนหั่น
- crash: `g_signal_connect_object` weak-ref อาร์กิวเมนต์ที่ 4 แต่ `App` เป็น struct ธรรมดา → ตายทันทีที่ cast ขึ้น
- wordmark ใช้สีเขียวที่แปลว่า live / สถานะพูดซ้ำสองที่ (การ์ด + strip)
- review เจออีก 6 จุด รวม **wordmark ในแอปมือถือยังเขียน QUOISE** (sed แบบ case-sensitive เดินผ่าน)
  และ **APK ทุกตัวใช้โลโก้ Flutter** (icon ถูกเขียนลงโฟลเดอร์ที่ `.gitignore` กันไว้)
  — ยืนยันหลังแก้ด้วยการเทียบ hash ของ `ic_launcher.png` ในไฟล์ APK กับต้นทาง ตรงกัน byte ต่อ byte

**Sender บน tablet 2304x1440 (PR #58 + branch ที่ยังไม่ merge)** เดิมดูแต่บนมือถือ:
การ์ดกว้าง 1900px ปุ่มยาวข้ามจอ — จำกัดเป็นคอลัมน์ 560, ลบ gear ที่ให้กรอก signalling server เอง,
ขีด 6 ขีดย้ายไปอยู่ใต้ตัวเลขแทนใต้ช่องว่าง

## เหลืออะไร / What's left

### 1. merge 2 branch ที่ CI เขียวแล้ว (ทำได้ทันที)

- **`fix/fullscreen-exit-geometry`** (`228f81c`) — receiver 3 commit:
  - ออกจาก fullscreen แล้วหน้าต่างก้นจมใต้ taskbar: GTK ถือ flag maximized ค้างข้าม fullscreen
    `gtk_window_maximize` เลยเป็น no-op → แก้เป็น unmaximize ก่อนแล้ว maximize (คนละ idle เพราะ
    เปลี่ยน window state สองครั้งติดกันเคยทำ win32 backend ตายด้วย `STATUS_HEAP_CORRUPTION`)
  - **เอา `GtkAspectFrame` ออก** เพราะมันวัดความสูงจากความกว้าง: log พิสูจน์ที่หน้าต่างกว้าง 1100
    `content wants at least 721` = (1100−92)/1.6 + chrome พอกว้าง 2560 มันขอ ~1700 ขณะที่จอมี 1392
    → strip หลุดขอบ **และลากย่อหน้าต่างไม่ได้** เพราะ minimum สูงกว่าจอ
    ราคาที่ยอมจ่าย: bezel ไม่รัดรูปวิดีโอแล้ว (letterbox ข้างใน ~5% บนจอ 16:9) ผู้ใช้เลือกเองว่าย่อได้สำคัญกว่า
  - **ยังไม่ได้ทดสอบกับของจริง** — build แล้วแต่ยังไม่ได้รันให้ผู้ใช้ลากย่อ/กด fullscreen ดู
- **`design/sender-one-group`** (`82bc322`) — sender 2 commit: การ์ดกับปุ่มรวมเป็นก้อนเดียวกลางจอ,
  ปุ่ม scan เปลี่ยนจากประโยคเป็นไอคอน QR (เก็บประโยคไว้เป็น tooltip + semantics label)
  — ทดสอบบน tablet แล้วด้วยภาพหน้าจอ ผ่าน

หมายเหตุ: `fix/fullscreen-exit-geometry` แตกมาจาก `design/sender-one-group` จึงมี commit `1917ee2` ติดมาด้วย

### 2. debug.keystore ยังไม่ทำงาน (ค้างกลางทาง)

อาการ: APK จาก CI แต่ละ build เซ็นคนละคีย์ → `adb install -r` ล้ม `INSTALL_FAILED_UPDATE_INCOMPATIBLE`
ต้องถอนก่อนลงทุกครั้ง ซึ่งขัดกับที่ `ci.yml` บอกว่า debug APK คือ "ตัวที่ maintainer ส่งให้คนลอง"

**ที่ทำไปแล้ว:** commit `sender/android/debug.keystore` (คีย์ debug มาตรฐานของ Android ไม่ลับ)
แล้วให้ `tools/scaffold-sender.sh` ก๊อปไป `~/.android/debug.keystore` เมื่อยังไม่มี

**ที่ตัดออกไปแล้ว:** ไม่ใช่เรื่อง "ไฟล์มีอยู่แล้วเลยไม่ก๊อป" — log ของ CI พิมพ์
`installed the committed debug key at ~/.android/debug.keystore` ครบทุก run ที่ตรวจ (3 run)
แต่ APK ก็ยังเซ็นคนละคีย์อยู่ดี

**สมมติฐานถัดไป:** Gradle ไม่ได้อ่าน `~/.android/debug.keystore` บน runner (น่าจะ `ANDROID_USER_HOME`
หรือ `ANDROID_SDK_HOME` ชี้ที่อื่น) **ขั้นต่อไป:** ประกาศ `signingConfigs.getByName("debug")` ใน
`android/app/build.gradle.kts` ให้ชี้ไฟล์ใน repo ตรงๆ ผ่านกลไก `pin()` ที่ `scaffold-sender.sh` มีอยู่แล้ว
(มันมีตัวตรวจว่า sed ไม่ match แล้ว fail ให้ด้วย) — ระวังอย่าไปแตะ `signingConfig` ของ release
ที่สคริปต์เดียวกันลบทิ้งและ fail build ถ้ายังเหลือ
**วิธีพิสูจน์:** build ใหม่แล้ว `adb install -r` ทับตัวเดิมโดยไม่ถอน ถ้าขึ้น `Success` คือจบ

### 3. ระบบอัปเดตยังตายสนิท (บล็อกที่ผู้ใช้)

`AIRCAST_UPDATE_PK[32] = { 0 }` ใน `receiver/update_check.c` — ยังไม่เคยสร้างคีย์ ทุก build ที่ปล่อยไป
จึงขึ้น "Update checks are not configured in this build" และกลไก signed manifest ทั้งชุดไม่เคยทำงานจริง

ต้องรันบน**เครื่อง offline** (`docs/threat-model.md:83` ห้าม private key อยู่บนเครื่องที่ต่อเน็ต
และห้ามเข้า GitHub Actions): `minisign -G -p aircast-update.pub -s aircast-update.key`
แล้วส่งมาแค่**บรรทัดที่สองของ `.pub`** — ส่วนที่เหลือ (decode 32 ไบต์ใส่ `update_check.c`,
พิมพ์ลง `receiver/README.md`, ทำ manifest) ทำต่อได้ทันที
ตอนนี้ยังไม่มีเครื่อง offline จึงข้ามไปก่อน (`winget install jedisct1.minisign` ถ้าจะลง minisign)

### 4. draft ค้าง 3 ตัว: v0.1.6 / v0.1.7 / v0.1.8

รอเซ็น `aircast-update.json` offline + เซ็น APK แล้ว publish (workflow จงใจจบเองไม่ได้)
hash ของ v0.1.8 MSI: `2f86b3602781da0a42d7fb3fc9197c9ca4f500e706269fff123e7ce9bc430857`
(verify กับ attestation แล้ว ผูกกับ `9d1f6a3` ref `refs/tags/v0.1.8`)

### 5. ยังไม่เคยทดสอบ / ยังไม่ได้ทำ

- **ปุ่มอัด (record)** ไม่เคยทดสอบ end-to-end เลย — ต้อง cast จริง กดอัด แล้วเปิดไฟล์ `.mkv` ดู
- **code-sign** ยังไม่มี cert: ทางฟรีคือ MSIX ขึ้น Microsoft Store (สมัครฟรีตั้งแต่ ก.ย. 2025
  Microsoft เซ็นให้เอง ไม่มี SmartScreen) แต่ MSIX ไม่มี custom action ต้องคิดเรื่อง registry prebuild ใหม่
  ทางจ่ายเงินคือ IV cert ~$219/ปี (EV ไม่คุ้มแล้วตั้งแต่ 2024 ที่มันเลิกข้าม SmartScreen ทันที)
- **gtk_init 1.2 วิ warm / ~4 วิ cold** เป็นก้อนใหญ่สุดที่เหลือ — ไล่แล้วทุก GDK flag
  (`GSK_RENDERER=gl`, ไม่มี dcomp, `GDK_DISABLE=d3d12`, `GDK_DISABLE=vulkan`) ไม่ขยับสักตัว
  ต้องใช้ profiler จริง (Procmon/WPA) ไม่ใช่งานเล็ก
- **macOS / iOS** ยังไม่เริ่ม: receiver build บน Linux ผ่านอยู่แล้วจึงน่าจะพอร์ตไม่ยาก แต่ต้องมี Mac + $99/ปี
  ส่วน iOS sender มีแผนเขียนไว้ครบใน `sender/ios/README.md` — แต่ ReplayKit ถูก deprecate ตั้งแต่ iOS 27
  ทางที่มีอยู่ตอนนี้จะต้องรื้อทิ้งเมื่อ ScreenCaptureKit บน iOS พร้อม

## วิธีเริ่ม / How to pick it up

```bash
# main + สอง branch ที่รอ merge
git log --oneline -1 main
git log --oneline main..fix/fullscreen-exit-geometry
git log --oneline main..design/sender-one-group

# CI ล่าสุดของ branch
gh run list --branch <branch> --limit 1
```

**กับดักที่เสียเวลาไปแล้ว — อ่านก่อนลงมือ**

- **msiexec ผ่าน Git Bash** ต้องมี `MSYS2_ARG_CONV_EXCL='*'` นำหน้า ไม่งั้น `/i` ถูกแปลงเป็น path
  แล้ว installer พ่น usage dialog เฉยๆ
- **ใช้ `/qb` ห้ามใช้ `/qn`** — แพ็กเกจเป็น `Scope="perMachine"` การติดตั้งเงียบยก UAC ไม่ได้
  ล้มด้วย `1730 → 1603`
- **จับภาพหน้าต่างใช้ `PrintWindow(hwnd, hdc, 2)`** — `SetForegroundWindow` ถูก Windows ปฏิเสธ
  เมื่อ process ไม่ได้อยู่หน้าสุด เคยถ่ายได้แต่หน้าต่างอื่น
- **APK จาก CI ต้องถอนก่อนลง** จนกว่าจะแก้ข้อ 2 เสร็จ
- **adb** อยู่ที่ `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Google.PlatformTools_*\platform-tools\adb.exe`
  ไม่ได้อยู่ใน PATH
- **`gh run download` ของ MSI 70 MB / APK 200 MB เกิน timeout 120 วิ** ต้องรัน background
- **เปิด PR แล้ว CI จะรันรอบใหม่** `mergeStateStatus` จะเป็น `BLOCKED` จนกว่ารอบนั้นจบ
  **เช็คผล merge ก่อน tag เสมอ** — เคยต่อคำสั่ง `merge && tag` ในบรรทัดเดียวแล้ว merge ถูกปัด
  แต่ tag เดินต่อ ไปลงคอมมิตเก่า (กู้ด้วยการลบ tag + cancel release run)
- **log ของ receiver** อยู่ที่ `%LOCALAPPDATA%\aircast\receiver.log` (ชื่อโฟลเดอร์ยังเป็น aircast)
  เขียนทับทุกครั้งที่เปิด และ**ถ้ารันจาก terminal มันจะ AttachConsole แล้วพ่นลง console แทนไฟล์** —
  ถ้าต้องการไฟล์ ให้ redirect `2> file` เอง
- CI ของ `sender (Android APK)` ใช้เวลา ~7 นาที เป็นตัวที่ช้าสุดเสมอ
