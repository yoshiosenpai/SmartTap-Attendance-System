# Smart RFID Student Attendance System
### with Excel/Sheets logging and parent notification

Hardware: **Cytron Maker ESP32** + expansion board, **Mifare RC522**, **I2C 16x2 LCD (0x27)**, **active buzzer**
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
   │   ┌───────────┐  GPIO  │  Maker ESP32    │          │   │   0x27     │
   │   │  Buzzer   │◄───────│                 │          │   └────────────┘
   │   └───────────┘        └────────┬────────┘          │
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

### 2.2 I2C 16x2 LCD → ESP32 (Maker Port)

| LCD backpack | ESP32 GPIO | Notes |
|---|---|---|
| `SDA` | **GPIO21** | Maker Port SDA |
| `SCL` | **GPIO22** | Maker Port SCL |
| `VCC` | **5V / VIN** | The LCD module needs 5 V for contrast |
| `GND` | **GND** | |

The backpack's I2C lines are open-drain and pulled to its own logic rail. The common
PCF8574 backpacks are 3.3 V-tolerant on SDA/SCL and work directly. If your LCD is dim,
it is under-powered — feed VCC from 5 V, not 3.3 V.

### 2.3 Buzzer → ESP32

| Buzzer | ESP32 GPIO | Notes |
|---|---|---|
| `+` / `VCC` / `I/O` | **GPIO25** | External **active** buzzer |
| `−` / `GND` | **GND** | |

Alternative: the Maker ESP32 already has an onboard **passive piezo on GPIO26** with a
hardware mute switch. To use it instead, set `PIN_BUZZER` to `26` and
`BUZZER_IS_ACTIVE` to `0` in the sketch, and make sure the mute switch is ON.

### 2.4 Pins that are now spoken for

```
 5  RC522 SS      18  RC522 SCK     21  LCD SDA      25  Buzzer
19  RC522 MISO    23  RC522 MOSI    22  LCD SCL      27  RC522 RST
```

Still free for expansion (LEDs, extra sensors): **2, 12, 13, 16, 17, 32, 33**
and input-only **34, 35, 36, 39**.

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
| Boot | 1 × 120 ms | `RFID Attendance` / `Starting up...` |
| Idle | — | `Tap Your Card...` / IP address or `WiFi...` |
| Card detected | 1 × 60 ms (instant) | `Card Tapped!` / the UID |
| Server says OK | 2 × 70 ms | `Welcome!` / student name |
| Repeat tap | 1 × 300 ms | `Already Tapped` / name |
| Unknown card | 3 × 250 ms | `Unknown Card` / the UID |
| No server | — | `Offline!` / `Not recorded` |

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
3. **LCD check.** If the screen is blank but backlit, turn the blue contrast pot on
   the backpack. If it is completely dead, the address is probably `0x3F` — change
   the `LiquidCrystal_I2C lcd(0x27, 16, 2);` line.
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
| RC522 version reads `0x00` / `0xFF` | Loose SPI wire, or the module is on 5 V |
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
