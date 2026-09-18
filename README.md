# Smart RFID Student Attendance System
### with Excel/Sheets logging and parent notification

Hardware: **Cytron Maker ESP32**, **Mifare RC522** (`RFID-RC522`), **I2C 16x2 LCD** (`DS-LCD-162A-I2C`, 0x27), **onboard piezo buzzer**
Backend: **Mosquitto MQTT** + **Node-RED** + **CSV/Excel** + **Telegram**

---

## 1. System architecture

```
   ┌──────────────── AT THE SCHOOL GATE ────────────────┐
   │                                                     │
   │   [Student card]                                    │
   │         │ 13.56 MHz                                 │
   │         ▼                                           │
   │   ┌───────────┐  SPI   ┌─────────────────┐          │
   │   │  RC522    │───────►│                 │          │
   │   └───────────┘        │                 │  I2C     │   ┌────────────┐
   │                        │  Cytron         │─────────►│   │ 16x2 LCD   │
   │    (onboard     GPIO26 │  Maker ESP32    │          │   │   0x27     │
   │     piezo) ◄───────────│                 │          │   └────────────┘
   │                        └────────┬────────┘          │
   │                                 │ Wi-Fi             │
   └─────────────────────────────────┼───────────────────┘
                                     │
                     MQTT  school/attendance/scan   (uplink)
                     MQTT  school/attendance/ack    (downlink -> LCD)
                                     │
   ┌─────────────────────────────────▼───────────────────────────────────┐
   │                     SERVER  (PC / Raspberry Pi)                     │
   │                                                                     │
   │   Mosquitto broker  ──►  Node-RED                                   │
   │                             │                                       │
   │        ┌────────────────────┼────────────────────┬─────────────┐    │
   │        ▼                    ▼                    ▼             ▼    │
   │   normalise +          match UID vs          build ACK     log       │
   │   duplicate guard      students.json         for LCD       unknown   │
   │                             │                    │          UIDs     │
   │            ┌────────────────┼──────────┐         │                   │
   │            ▼                ▼          ▼         ▼                   │
   │      attendance.csv    Telegram     Google    back to ESP32          │
   │       (open in         Bot API      Sheets    over MQTT              │
   │        Excel)             │        (optional)                        │
   └───────────────────────────┼──────────────────────────────────────────┘
                               ▼
                    📱 "Ali Bin Ahmad has arrived
                        at school at 07:58:12"
```

**Data flow in one sentence:** card → UID → JSON over MQTT → Node-RED matches the student,
appends a CSV row, messages the parent on Telegram, and replies to the ESP32 so the LCD
shows *Welcome!* or *Unknown Card*.

---

## 2. Pin mapping

### 2.1 RC522 RFID reader → ESP32 (SPI / VSPI)

Module: **Cytron `RFID-RC522`** Mifare kit. The 8-pin header runs down one edge in this
order — `SDA, SCK, MOSI, MISO, IRQ, GND, RST, 3.3V` — and it ships **loose, not soldered**
(see §2.6).

| RC522 pin | ESP32 GPIO | Notes |
|---|---|---|
| `SDA` / `SS` / `NSS` | **GPIO5** | Chip select. See the boot note below. |
| `SCK` | **GPIO18** | VSPI clock |
| `MOSI` | **GPIO23** | VSPI data out |
| `MISO` | **GPIO19** | VSPI data in |
| `IRQ` | *not connected* | Not used — the sketch polls instead |
| `GND` | **GND** | |
| `RST` | **GPIO27** | |
| `3.3V` | **3V3** | ⚠️ **3.3 V ONLY — 5 V destroys the RC522** |

### 2.2 I2C 16x2 LCD → ESP32

Module: **Cytron `DS-LCD-162A-I2C`** — I2C address `0x27`, supply 5 V, blue backlight.
The backpack has a 4-pin header in this order:

| LCD backpack pin | ESP32 | Notes |
|---|---|---|
| `GND` | **GND** | |
| `VCC` | **3.3V** *(try first)* or **5V** | see §2.6 — this choice matters |
| `SDA` | **GPIO21** | |
| `SCL` | **GPIO22** | |

Address `0x27` is confirmed on the product page, so the `LiquidCrystal_I2C lcd(0x27, 16, 2);`
line in the sketch is already correct for your unit.

### 2.3 Buzzer

**Using the onboard buzzer — no wiring at all.**

The Maker ESP32 has a **passive piezo hard-wired to GPIO26**, so there is nothing to
connect. The sketch ships configured for it:

```cpp
#define PIN_BUZZER       26
#define BUZZER_IS_ACTIVE  0      // passive piezo -> driven with tone()
```

⚠️ **The board has a hardware mute switch next to the buzzer.** If it is off you hear
nothing regardless of the code. Check this before debugging anything else.

**Passive vs active — why it changes the code.** A passive piezo has no oscillator
inside. Holding the pin HIGH deflects the disc once and leaves it there: a faint click,
then silence. It needs a square wave, which is what `tone()` produces. An active buzzer
is the opposite — it oscillates on its own, so `digitalWrite()` is correct and `tone()`
would be wrong. The sketch picks the right one from `BUZZER_IS_ACTIVE`.

**If you'd rather use an external active buzzer** (louder, better through an enclosure):

| Buzzer | ESP32 GPIO |
|---|---|
| `+` / `VCC` / `I/O` | **GPIO25** |
| `−` / `GND` | **GND** |

then set `PIN_BUZZER` back to `25` and `BUZZER_IS_ACTIVE` to `1`.

**Test it first:** upload [`firmware/BuzzerTest/BuzzerTest.ino`](firmware/BuzzerTest/BuzzerTest.ino)
before touching the main project — see §3.6.

### 2.4 Pins that are now spoken for

```
 5  RC522 SS      18  RC522 SCK     21  LCD SDA      26  onboard buzzer
19  RC522 MISO    23  RC522 MOSI    22  LCD SCL      27  RC522 RST
```

Still free for expansion (LEDs, extra sensors): **2, 12, 13, 16, 17, 25, 32, 33**
and input-only **34, 35, 36, 39**. GPIO25 is free again now that the buzzer is onboard.

### 2.5 Safety rules that shaped these choices

- **Never apply 5 V to any GPIO.** The ESP32 is 3.3 V logic.
- **GPIO 6–11 are wired to the internal flash — never use them.**
- **GPIO 34, 35, 36, 39 are input-only** — no output, no internal pull-ups.
- **GPIO5 is a boot-strapping pin** and must be HIGH at power-up. The RC522's SS line
  idles high, so this works — and it is what nearly every RC522 tutorial uses. If your
  board ever refuses to boot with the reader attached, move `SS` to **GPIO13** and
  change `PIN_RC522_SS` to match. Nothing else needs to change.
- Other strapping pins to be careful with: **GPIO 0, 2, 4, 12, 15**.
- The user button is on **GPIO4** (active LOW) if you want a manual override later.

### 2.6 Notes specific to these two Cytron modules

#### (a) The RC522 header is not soldered

The kit ships the 8-pin header **loose in the bag**. You have to solder it yourself before
anything works. Two choices:

- **Straight header** — pins point up, jumper wires plug in from above. Easiest to solder.
- **Right-angle header** — the module lies flat behind a panel. Better for a real gate
  enclosure, slightly fiddlier.

Solder all 8 pins even if you never use `IRQ`; a header with one leg free rocks and
cracks the joints. A cold joint on `MISO` is the single most common reason
`PCD_DumpVersionToSerial()` prints `0x00`.

The kit's white card and keychain fob are Mifare Classic 1K, which give a **4-byte UID =
8 hex characters** (e.g. `A1B2C3D4`). The `uidToHex()` function loops over `uid.size`, so
7-byte UIDs from newer tags also work without any code change.

#### (b) The LCD runs at 5 V but the ESP32 does not — read this before wiring

The product page lists the LCD supply as **5 V**. The PCF8574 backpack has two ~4.7 kΩ
pull-up resistors from `SDA`/`SCL` to its own `VCC`. So if you feed `VCC` with 5 V, the
I2C lines idle at **5 V** — and the ESP32's absolute maximum on any GPIO is 3.6 V.

This is the one place where the standard tutorial wiring is out of spec. Four options,
best first:

| | Wiring | Verdict |
|---|---|---|
| **1** | `VCC` → **3.3 V**, SDA/SCL direct | **Try this first.** Fully in spec, zero extra parts. The PCF8574 works down to 2.5 V. Turn the contrast pot — if the characters are crisp, you are done. Backlight is a little dimmer. |
| **2** | `VCC` → **5 V**, SDA/SCL through a bi-directional logic level converter | The correct answer if option 1 is too faint. One extra RM3 part. Use this for a system that has to run all school year. |
| **3** | `VCC` → **5 V**, desolder the two pull-up resistors on the backpack, enable the ESP32's internal pull-ups | Free, in spec, permanent mod to the module. Fine at 100 kHz with short wires. |
| **4** | `VCC` → **5 V**, SDA/SCL direct | What most tutorials show. It usually survives — the 4.7 kΩ resistors limit current into the ESP32's clamp diodes to ~360 µA — but it is outside the datasheet and it stresses the pin. Your call; I would not ship a school installation this way. |

Try option 1 tonight. It costs nothing and most of these modules are perfectly readable at
3.3 V once the pot is tuned. If yours is not, go to option 2.

The RC522 has no such problem — it is a native 3.3 V part and connects straight to the
ESP32. The onboard buzzer needs no wiring at all, so it sidesteps the question entirely.

#### (c) Where 3.3 V and 5 V come from

If your expansion board has per-GPIO 3-pin headers (signal / V / GND) with a **VCC
selection jumper**, set it deliberately: the RC522 must be on the **3.3 V** setting. If
the jumper is shared across the whole board and set to 5 V, power the RC522 from a
separate 3.3 V pin instead — 5 V will destroy it.

---

## 3. Firmware

Files: [`firmware/SmartRFID_Attendance.ino`](firmware/SmartRFID_Attendance.ino),
[`firmware/secrets.h`](firmware/secrets.h)

### 3.1 Libraries

Arduino IDE → **Tools → Manage Libraries**, then install:

| Library | Author | What it does |
|---|---|---|
| **MFRC522** | GithubCommunity | Drives the RC522 over SPI; gives you `uid.uidByte` |
| **LiquidCrystal I2C** | Frank de Brabander | 16x2 LCD over two wires |
| **PubSubClient** | Nick O'Leary | MQTT publish/subscribe |
| **ArduinoJson** (7.x) | Benoit Blanchon | Builds the payload, parses the ACK |

`WiFi.h`, `SPI.h`, `Wire.h` and `time.h` come with the ESP32 board package — nothing
to install for those.

Board setting: **ESP32 Dev Module**, 115200 baud.

### 3.2 What to edit before uploading

Everything private lives in `secrets.h` — the `.ino` needs no edits to run:

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
#define MQTT_HOST "192.168.1.100"
```

⚠️ Changing Wi-Fi credentials, the MQTT host, or any GPIO define means re-uploading.
Keep `secrets.h` out of GitHub and out of anything you hand to students.

### 3.3 How the sketch stays non-blocking

`loop()` is five one-line calls, and none of them waits:

```cpp
void loop() {
  serviceWifi();     // retries the AP every 10 s if the link dropped
  serviceMqtt();     // retries the broker every 5 s, pumps incoming ACKs
  serviceRfid();     // polls the reader every 80 ms
  serviceBuzzer();   // advances the beep pattern one step
  serviceLcd();      // returns to the idle screen when the hold time expires
}
```

Each service compares `millis()` against its own timestamp and returns immediately if
it is not due. There is exactly one `delay(100)` in the whole sketch — in `setup()`,
so the serial port settles before the first print. It never runs again.

The one call that *can* stall briefly is `mqtt.connect()` when the broker is
unreachable; `mqtt.setSocketTimeout(2)` caps that at ~2 seconds and it is only
attempted once every 5 seconds, so card reading stays responsive even with the
server switched off.

### 3.4 Payload sent on each tap

Topic `school/attendance/scan`:

```json
{ "device": "gate-01", "uid": "A1B2C3D4", "timestamp": "2026-09-17T07:58:12+08:00" }
```

If NTP has not synced yet, `timestamp` is `""` and Node-RED stamps the record with its
own clock instead — no record is ever lost to a missing time sync.

### 3.5 Buzzer and LCD feedback

| Event | Buzzer | LCD |
|---|---|---|
| Boot | two-note chirp | `RFID Attendance` / `Starting up...` |
| Idle | — | `Tap Your Card...` / IP address or `WiFi...` |
| Card detected | 1 × 60 ms (instant) | `Card Tapped!` / the UID |
| Server says OK | 2 × 70 ms | `Welcome!` / student name |
| Repeat tap | 1 × 300 ms | `Already Tapped` / name |
| Unknown card | 3 × 250 ms | `Unknown Card` / the UID |
| No server | — | `Offline!` / `Not recorded` |

### 3.6 Testing the onboard buzzer on its own

Upload [`firmware/BuzzerTest/BuzzerTest.ino`](firmware/BuzzerTest/BuzzerTest.ino) —
no libraries needed. Open Serial Monitor at **115200**. You should hear a two-note boot
chirp immediately, then get a menu:

| Key | Plays | Used in the project for |
|---|---|---|
| `1` | one short chirp | card detected |
| `2` | two rising notes | tap accepted |
| `3` | one flat mid note | duplicate tap |
| `4` | three low notes | unknown card |
| `5` | boot chirp | power-on |
| `6` | sweep 1 kHz → 4.5 kHz | **finding your loudest frequency** |
| `7` | C major scale | proves pitch control works |
| `0` | silence | panic button |

**Run option 6 first.** Piezo discs have a mechanical resonance, and yours will be
noticeably louder at one point in the sweep — usually somewhere between 2 and 3 kHz.
Note which step sounds loudest and set `BUZZER_TONE_HZ` in the main sketch to match.
That single change is worth more volume than anything else you can do in software.

**Silent?** In this order: (1) the **mute switch**, (2) is `PIN_BUZZER` really `26`,
(3) did you leave `BUZZER_IS_ACTIVE` at `1` — a passive piezo driven by `digitalWrite()`
clicks once and then goes quiet.

The note sequencer in the test sketch is the same non-blocking pattern the main project
uses, so anything you like here transfers straight across.

---

## 4. Node-RED backend

Files: [`node-red/attendance-flow.json`](node-red/attendance-flow.json),
[`node-red/students.json`](node-red/students.json),
[`node-red/google-apps-script.gs`](node-red/google-apps-script.gs)

### 4.1 Extra npm nodes: none required

The flow deliberately uses **only core Node-RED nodes**, so `Import` just works on a
fresh install. Telegram is called with a plain `http request` node against the Bot API
rather than a contrib node.

Optional upgrades, if you want them later:

| Package | Why you might add it |
|---|---|
| `node-red-contrib-excel-port` | Write a real `.xlsx` instead of `.csv` |
| `node-red-node-xlsx` | Convert a JS array to an xlsx buffer |
| `node-red-contrib-telegrambot` | Nicer Telegram UX: buttons, inbound commands |
| `node-red-contrib-google-sheets` | Native Sheets writes via OAuth (heavier setup) |
| `node-red-node-email` | Email parents over SMTP instead of Telegram |

Install with **Menu → Manage palette → Install**.

### 4.2 The flow, node by node

**Section 1 — student database**
`inject (once)` → `file in: students.json` → `json` → `change: → flow.students` → `debug`

Loads the registry into flow context half a second after deploy. Edit `students.json`
and click the inject button to reload — no restart.

**Section 2 — scan ingress**
`mqtt in (school/attendance/scan)` ┐
`http in (POST /attendance)`       ├→ `Normalise + dedupe` → `Match student` → `switch: result?`
`inject (simulate tap)`            ┘

- **Normalise + dedupe** — accepts an object or a string, strips the UID to uppercase
  hex, falls back to the server clock if the ESP32 timestamp is blank, and flags a
  repeat tap of the same card at the same door within 5 minutes.
- **Match student** — looks the UID up in `flow.students`, sets `msg.result` to
  `ok` / `duplicate` / `unknown`, and pre-formats the date and time.
- **switch** — three outputs, one per result.

**Section 3 — known student (result = ok)**
- `Build CSV row` → `file: attendance/attendance.csv` (append, creates the folder)
- `Build Telegram message` → `http request` → Telegram Bot API
- `Build Sheets row` → `http request` → your Apps Script Web App *(skipped if unset)*

**Section 4 — answer the reader (all three results)**
`Build LCD ACK` → `switch: came in over HTTP?` → `http response` **or** `mqtt out`

The same ACK builder serves MQTT and HTTP scans; the switch picks the return path by
testing whether `msg.res` exists.

**Section 5 — unknown cards and errors**
`Log unknown UID` → `file: attendance/unknown_uids.csv`, plus a `catch` → `debug`.
This file is how you enrol a new student: tap the card, copy the UID out of the CSV,
paste it into `students.json`.

**Section 6 — bench test**
An inject node that pushes a fake scan through the whole chain. You can build and test
the entire backend before the hardware is even wired.

### 4.3 Key JavaScript: the student lookup

```js
const db  = flow.get('students') || {};
const rec = db[msg.scan.uid] || null;

msg.student = rec;
msg.result  = !rec ? 'unknown'
                   : (msg.scan.duplicate ? 'duplicate' : 'ok');
```

### 4.4 Key JavaScript: the duplicate guard

```js
const COOLDOWN_MS = 5 * 60 * 1000;
const seen = flow.get('lastSeen') || {};
const key  = uid + '@' + (p.device || 'unknown');
const dup  = seen[key] && (Date.now() - seen[key] < COOLDOWN_MS);
seen[key]  = Date.now();
flow.set('lastSeen', seen);
```

Without this, one student holding the card against the reader would message their
parent several times.

### 4.5 Key JavaScript: CSV row

```js
const q = v => '"' + String(v ?? '').replace(/"/g, '""') + '"';
msg.payload = [q(f.date), q(f.time), q(s.studentId), q(s.name),
               q(s.className), q(msg.scan.uid), q(msg.scan.device)].join(',');
```

Quoting is doubled so a name like `Tan, Wei Ming` cannot shift the columns.

---

## 5. Setup, step by step

### 5.1 Broker

```bash
sudo apt install mosquitto mosquitto-clients
```

Windows: install from mosquitto.org, then allow port 1883 through the firewall.
For a school network, add a username/password rather than running it anonymously.

### 5.2 Node-RED

1. Copy `students.json` into your Node-RED **userDir** (`~/.node-red`, or
   `%USERPROFILE%\.node-red` on Windows).
2. Create `attendance/attendance.csv` in the same folder with **one header line**:
   ```
   Date,Time,StudentID,Name,Class,UID,Device
   ```
   (and `attendance/unknown_uids.csv` with `Date,Time,UID,Device`).
3. Node-RED → **Menu → Import → clipboard** → paste `attendance-flow.json` → **Import**.
4. Double-click the **Local Broker** config node, set your broker IP, **Deploy**.

### 5.3 Telegram

1. Message **@BotFather** → `/newbot` → copy the token.
2. Each parent messages your bot once (Telegram will not let a bot open a conversation),
   then send **@userinfobot** to get their numeric chat id.
3. Put the chat id in `students.json` as `parentChatId`.
4. Set the token as an environment variable **before** starting Node-RED:
   ```bash
   export TELEGRAM_BOT_TOKEN="123456:ABC..."
   ```
   ⚠️ The token is a password for your bot. Never paste it into the flow you export
   or share, and never commit it.

### 5.4 Google Sheets (optional)

Follow the header comment in `node-red/google-apps-script.gs`, then:

```bash
export SHEETS_WEBAPP_URL="https://script.google.com/macros/s/..../exec"
```

If this variable is missing, the Sheets branch quietly does nothing — the CSV and
Telegram paths are unaffected.

---

## 6. Testing

### 6.1 Backend alone, before wiring anything

Click the **Simulate tap A1B2C3D4** inject node. Expected:
- `attendance.csv` grows by one row
- the `telegram result` debug shows `{"ok":true,...}`
- the ACK appears on `school/attendance/ack`

Watch the ACK from a terminal:
```bash
mosquitto_sub -h localhost -t 'school/attendance/#' -v
```

### 6.2 Hardware bring-up, in this order

1. **Power only.** Open Serial Monitor at 115200. You should see the banner, one
   120 ms chirp, and the LCD reading `RFID Attendance / Starting up...`.
2. **Reader check.** `PCD_DumpVersionToSerial()` should print firmware **0x92** or
   **0x91**. `0x00` or `0xFF` means wiring — re-check MOSI/MISO/SCK and confirm the
   RC522 is on **3.3 V**.
3. **LCD check.** The sketch runs an I2C scan at boot and prints what it finds:
   - `device at 0x27` → wiring is good. A blank-but-backlit screen is now purely a
     contrast problem: turn the small blue pot on the backpack.
   - `device at 0x3F` → change the `LiquidCrystal_I2C lcd(0x27, 16, 2);` line.
   - `nothing found` → SDA/SCL swapped, backpack unpowered, or a loose wire.
4. **Network check.** The idle screen should change from `WiFi...` → `Server...` →
   the board's IP address.
5. **Tap a card.** Expect: one short beep immediately, `Card Tapped!` + the UID, then
   within a second `Welcome!` + the student name and two short beeps.
6. **Tap the same card again.** Expect one long beep and `Already Tapped` — and **no**
   second Telegram message.
7. **Tap an unregistered card.** Expect three long beeps, `Unknown Card`, and a new
   line in `unknown_uids.csv`.
8. **Pull the network cable from the server.** Expect `Offline!` on the LCD, and the
   reader still beeping — the sketch must never freeze.

### 6.3 Enrolling a real student

Tap the new card → open `attendance/unknown_uids.csv` → copy the UID → add a block to
`students.json` → click the **Load student DB** inject → tap again. It should now say
`Welcome!`.

---

## 7. If something goes wrong

| Symptom | Likely cause |
|---|---|
| RC522 version reads `0x00` / `0xFF` | Cold solder joint on the header (most often MISO), loose SPI wire, or the module is on 5 V |
| LCD faint at 3.3 V even at full pot | Expected on some units — move to §2.6(b) option 2 |
| Reader works on the bench, dies in the enclosure | 3.3 V rail sagging — power the ESP32 from a proper USB-C supply, not a laptop hub |
| LCD blank but lit | Contrast pot, or wrong I2C address (`0x27` vs `0x3F`) |
| LCD shows `Server...` forever | Wrong `MQTT_HOST`, firewall on 1883, or broker requires auth |
| MQTT `state=-2` in serial | Cannot reach the broker at all — check IP and port |
| MQTT `state=5` | Bad username/password |
| Card reads but nothing logs | Node-RED not deployed, or topic mismatch |
| Telegram debug shows `"ok":false` | Wrong token, or the parent never messaged the bot first |
| Names shifted across CSV columns | You edited the CSV builder and dropped the quoting |

### Rollback

If an upgrade misbehaves, go back to your last known-good version:

- **Firmware** — re-upload the previous `.ino`. Keep dated copies
  (`SmartRFID_Attendance_v1_working.ino`) before each change.
- **Node-RED** — the editor keeps deploy history under the flow's context menu, or
  re-import this `attendance-flow.json` and redeploy.
- **Student data** — `students.json` is plain text; keep a copy before bulk edits.

Change one thing at a time, test, then move on. That is the fastest route through an
embedded project.

---

## 8. Where to take it next

Each of these is a single, self-contained upgrade:

- **Late arrivals** — compare the time against a cut-off in the Match node and send a
  different Telegram message after 07:30.
- **Check-out** — a second reader (`device: "gate-02"`) and a `direction` field, so
  parents get "left school" alerts too.
- **Daily summary** — a Node-RED `inject` at 18:00 that emails the class teacher the
  day's CSV.
- **Offline buffer** — queue scans in ESP32 NVS while MQTT is down, flush on reconnect.
- **Onboard status LEDs** — the Maker ESP32 has active-HIGH LEDs on GPIO 2, 12, 13, 16,
  17, 32, 33; wire green = accepted, red = unknown.
