# Node-RED Setup Guide
### Backend for the Smart RFID Student Attendance System

This guide is self-contained. Follow it top to bottom and you will have a working
backend **before** you touch the ESP32 — the flow ships with a simulate button so you
can test the whole chain with no hardware at all.

**Time needed:** about 40 minutes the first time.

> ### Your setup, concretely
>
> | | |
> |---|---|
> | Server PC | Runs **Windows Mobile Hotspot** (SSID `Cytron`) **and** Mosquitto **and** Node-RED |
> | `<SERVER_IP>` | **`192.168.137.1`** — everywhere this guide says `<SERVER_IP>`, use this |
> | Node-RED editor | `http://192.168.137.1:1880` |
> | Cards enrolled | `B96DF306` `97513A25` `8EEB2907` `295E5514` |
> | Telegram | One chat id on all four students, for testing |
>
> Because the PC is also the access point, **skip the static-IP advice in §2** —
> Windows pins itself to `192.168.137.1` whenever the hotspot is on, and that address
> never changes. Follow the **Windows** branch at every step, and do not skip the
> firewall rule in §4.

| File | What it is |
|---|---|
| `attendance-flow.json` | The flow. Import this into Node-RED. |
| `students.json` | Your student registry (UID → name, ID, parent). |
| `google-apps-script.gs` | Optional Google Sheets bridge. |

---

## Contents

1. [What you are building](#1-what-you-are-building)
2. [Prerequisites](#2-prerequisites)
3. [Install Node-RED](#3-install-node-red)
4. [Install the MQTT broker](#4-install-the-mqtt-broker)
5. [Place the project files](#5-place-the-project-files)
6. [Import the flow](#6-import-the-flow)
7. [Set the file paths](#7-set-the-file-paths--the-step-people-skip)
8. [Point the flow at your broker](#8-point-the-flow-at-your-broker)
9. [Set up the Telegram bot](#9-set-up-the-telegram-bot)
10. [Google Sheets (optional)](#10-google-sheets-optional)
11. [Test with no hardware](#11-test-with-no-hardware)
12. [Connect the ESP32](#12-connect-the-esp32)
13. [Day-to-day operation](#13-day-to-day-operation)
14. [Make it survive a reboot](#14-make-it-survive-a-reboot)
15. [Before it goes live](#15-before-it-goes-live)
16. [Troubleshooting](#16-troubleshooting)

---

## 1. What you are building

```
  ESP32 ──MQTT──►  Mosquitto  ──►  Node-RED  ──┬──►  attendance.csv  (open in Excel)
   at the           broker                     ├──►  Telegram to the parent
    gate                                       ├──►  Google Sheet   (optional)
     ▲                                         │
     └───────────── ACK for the LCD ◄──────────┘
```

Node-RED is the brain: it receives a card UID, works out who it belongs to, writes the
log, messages the parent, and tells the reader what to print on its LCD.

Everything runs on one machine — a Raspberry Pi, an old laptop, or the school's PC.
Mosquitto and Node-RED sit side by side on it.

---

## 2. Prerequisites

| | |
|---|---|
| **A machine that stays on** | Raspberry Pi 3/4/5, a mini PC, or any always-on Windows/Linux box. A Pi 4 is plenty. |
| **A fixed IP address** | ✅ Already solved for you: Windows Mobile Hotspot always assigns itself `192.168.137.1`. Nothing to configure. |
| **Same network as the ESP32** | ✅ Already solved: the ESP32 joins the PC's own hotspot, so they are always on the same subnet. This also sidesteps school-network client isolation entirely. |
| **Node.js 18 or newer** | Installed in step 3. |

**Your `<SERVER_IP>` is `192.168.137.1`.** Confirm it with the hotspot switched on:

```powershell
ipconfig | Select-String "IPv4"
```

Look for the adapter named "Local Area Connection* N" — that is the hotspot. If it
shows anything other than `192.168.137.1`, the hotspot is off; turn it on and re-check.

---

## 3. Install Node-RED

### Raspberry Pi / Debian / Ubuntu

The official script installs Node.js, Node-RED, and a systemd service in one go:

```bash
bash <(curl -sL https://raw.githubusercontent.com/node-red/linux-installers/master/deb/update-nodejs-and-nodered)
```

Accept the defaults. Then:

```bash
sudo systemctl enable nodered.service
sudo systemctl start nodered.service
```

### Windows

1. Install **Node.js LTS** from nodejs.org (take the default options).
2. Open **Command Prompt** and run:

```bash
npm install -g --unsafe-perm node-red
```

3. Start it by typing `node-red` in a Command Prompt. **Leave that window open** —
   closing it stops Node-RED. (Section 14 shows how to run it as a proper service.)

### macOS

```bash
brew install node
npm install -g --unsafe-perm node-red
node-red
```

### Check it worked

Open a browser on the same machine:

```
http://localhost:1880
```

You should see the Node-RED editor — a blank canvas with a palette on the left. From
another device on the hotspot use `http://192.168.137.1:1880`.

**Read the startup log before you move on.** It prints two lines you will need:

```
Settings file  : /home/pi/.node-red/settings.js
User directory : /home/pi/.node-red
```

That **user directory** is where everything in step 5 goes. On Windows it is usually
`C:\Users\<you>\.node-red`.

---

## 4. Install the MQTT broker

Node-RED does **not** include a broker. Mosquitto is the standard choice.

### Raspberry Pi / Debian / Ubuntu

```bash
sudo apt update && sudo apt install -y mosquitto mosquitto-clients
```

### Windows

Download the installer from mosquitto.org and run it. It installs as a Windows service.
Also tick the option to install the **clients** — you want `mosquitto_sub` for testing.

### ⚠️ Now configure it, or the ESP32 cannot connect

This trips up almost everyone. **Mosquitto 2.x refuses remote connections out of the
box**: with no config it listens on localhost only, and it denies anonymous clients.
Node-RED on the same machine will connect fine, and the ESP32 will fail forever with
`state=-2`.

**Option A — quick, for a closed lab network:**

Create `/etc/mosquitto/conf.d/attendance.conf` (Linux) or edit
`C:\Program Files\mosquitto\mosquitto.conf` (Windows):

```
listener 1883 0.0.0.0
allow_anonymous true
```

**Option B — with a password, for anything real:**

```
listener 1883 0.0.0.0
allow_anonymous false
password_file /etc/mosquitto/passwd
```

Then create the user:

```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd attendance
```

It prompts twice for a password. Put the same username and password into the ESP32's
`secrets.h` (`MQTT_USER` / `MQTT_PASS`) and into the Node-RED broker node in step 8.

Restart the broker:

```bash
sudo systemctl restart mosquitto
```
```powershell
Restart-Service mosquitto
```

### ⚠️ Windows Mobile Hotspot: the firewall rule is not optional

Your ESP32 reaches the PC over the hotspot adapter. Windows treats that adapter as its
own network and, by default, **blocks inbound port 1883 on it**. Everything will look
healthy from the PC itself — Node-RED connects to `localhost` fine — while the ESP32
retries forever with `state=-2`.

Run this in PowerShell **as Administrator**:

```powershell
New-NetFirewallRule -DisplayName "MQTT 1883" -Direction Inbound -Protocol TCP -LocalPort 1883 -Action Allow -Profile Any
```

`-Profile Any` matters: the hotspot adapter is often classified Public, and a rule that
only covers Private profiles will not apply to it.

To reach the Node-RED editor from another device on the hotspot, add 1880 too:

```powershell
New-NetFirewallRule -DisplayName "Node-RED 1880" -Direction Inbound -Protocol TCP -LocalPort 1880 -Action Allow -Profile Any
```

**Two more hotspot habits worth forming:**

- **Turn the hotspot on before powering the ESP32.** If it is off at boot the reader sits
  on `WiFi...` until it comes back.
- **Stop the PC sleeping.** Windows shuts the Mobile Hotspot down when the machine
  sleeps, and some builds time it out after a period with no clients. Settings → System →
  Power → set *Screen and sleep* to Never for a live demo. A reader that "worked
  yesterday" and is dead this morning is almost always this.

### Confirm the broker is reachable

In one terminal:
```bash
mosquitto_sub -h 192.168.137.1 -t 'test/#' -v
```
In another:
```bash
mosquitto_pub -h 192.168.137.1 -t 'test/hello' -m 'it works'
```

Add `-u attendance -P yourpassword` to both if you chose Option B. If `it works` appears
in the first terminal **using the IP address, not `localhost`**, the broker is ready.
If it only works with `localhost`, your `listener` line has not taken effect.

---

## 5. Place the project files

Copy into the **user directory** from step 3:

```
~/.node-red/                     (or C:\Users\<you>\.node-red\)
├── students.json                ← copy it here
└── attendance/                  ← create this folder
    ├── attendance.csv           ← create it, with the header line below
    └── unknown_uids.csv         ← create it, with the header line below
```

**Create the two CSV files with their header rows.** The flow only appends rows; it
never writes a header, so if you skip this your columns have no titles.

`attendance/attendance.csv` — first line exactly:
```
Date,Time,StudentID,Name,Class,UID,Device
```

`attendance/unknown_uids.csv` — first line exactly:
```
Date,Time,UID,Device
```

Quick way on Linux/macOS:

```bash
cd ~/.node-red && mkdir -p attendance
echo "Date,Time,StudentID,Name,Class,UID,Device" > attendance/attendance.csv
echo "Date,Time,UID,Device" > attendance/unknown_uids.csv
```

PowerShell:

```powershell
cd $env:USERPROFILE\.node-red; mkdir attendance -Force
"Date,Time,StudentID,Name,Class,UID,Device" | Out-File attendance\attendance.csv -Encoding utf8
"Date,Time,UID,Device" | Out-File attendance\unknown_uids.csv -Encoding utf8
```

### Edit `students.json`

**Already done for you** — your four real cards are in it, each with your Telegram chat
id so every tap messages you while you test. You only need to replace the placeholder
names, student ids and classes.

The **key is the card UID** exactly as the ESP32 prints it: uppercase hex, no spaces,
no colons.

```json
{
  "A1B2C3D4": {
    "studentId": "S2026-001",
    "name": "Ali Bin Ahmad",
    "className": "5 Bestari",
    "parentName": "Encik Ahmad",
    "parentChatId": "111111111",
    "parentPhone": "+60123456789"
  }
}
```

Don't know the UIDs yet? Leave the samples in for now — section 13 shows how to read
real cards and enrol them.

---

## 6. Import the flow

1. Open `http://192.168.137.1:1880`.
2. **Menu (☰) → Import**.
3. Open `attendance-flow.json` in a text editor, copy everything, paste it into the box.
4. Click **Import**, then **Deploy** (top right, red button).

A new tab called **RFID Attendance** appears with six labelled sections. Every node is a
core Node-RED node, so there is nothing to install from the palette — the import will
not show any "unknown node" errors.

You will see red/yellow status under some nodes until you finish steps 7 and 8. That is
expected.

---

## 7. Set the file paths — the step people skip

The flow ships with **relative** paths (`students.json`, `attendance/attendance.csv`).
Relative paths resolve against the Node-RED process's **working directory**, which is
not always the user directory — on Windows it is wherever you happened to run `node-red`
from. If your CSV never appears, this is why.

**The reliable fix: make all three paths absolute.**

Double-click each node below and put in the full path. **Use forward slashes on
Windows** — they work fine in Node-RED and avoid backslash-escaping headaches. Replace
`YourName` with your actual Windows username.

| Node | Section | Set `Filename` to |
|---|---|---|
| `students.json` | 1 | `C:/Users/YourName/.node-red/students.json` |
| `attendance.csv` | 3 | `C:/Users/YourName/.node-red/attendance/attendance.csv` |
| `unknown_uids.csv` | 5 | `C:/Users/YourName/.node-red/attendance/unknown_uids.csv` |

(On a Raspberry Pi these would be `/home/pi/.node-red/...` instead.)

Not sure of your username? The Node-RED startup log prints a `User directory` line —
copy the path from there and swap the backslashes for forward slashes.

**Deploy** after editing.

---

## 8. Point the flow at your broker

1. Double-click the **MQTT scan in** node (section 2).
2. Next to **Server**, click the pencil ✏️ icon.
3. Set **Server** to `192.168.137.1` and **Port** to `1883`.
   - Using `localhost` works only if Node-RED and Mosquitto are on the same machine.
     Putting the real IP here is never wrong, so just use it.
4. If you chose password Option B: **Security** tab → username and password.
5. **Update** → **Done** → **Deploy**.

The MQTT nodes should now show a green **connected** dot. If not, jump to section 16.

---

## 9. Set up the Telegram bot

Telegram is free, instant, and needs no phone-number verification per parent.

### Create the bot

1. In Telegram, search for **@BotFather** and start a chat.
2. Send `/newbot`, give it a name and a username ending in `bot`.
3. BotFather replies with a **token** like `123456789:AAF...`. This is a password for
   your bot — treat it like one.

### Get each parent's chat ID

A bot **cannot message someone first** — Telegram blocks that. So for every parent:

1. The parent opens your bot and presses **Start** (send them the `t.me/yourbot` link).
2. They send **@userinfobot** any message; it replies with their numeric ID.
3. Put that number in `students.json` as `parentChatId`.

This one-time opt-in is a feature, not a limitation — it means no parent gets messages
they did not agree to.

### Give Node-RED the token

The flow reads the token from an **environment variable**, so it never gets saved into
the flow file you export or share.

**Linux / Raspberry Pi (systemd):**

```bash
sudo systemctl edit nodered.service
```

Add:

```
[Service]
Environment="TELEGRAM_BOT_TOKEN=123456789:AAF..."
```

Save, then:

```bash
sudo systemctl daemon-reload && sudo systemctl restart nodered
```

**Windows:**

```powershell
setx TELEGRAM_BOT_TOKEN "123456789:AAF..."
```

Then **close and reopen** the Command Prompt and start `node-red` again — `setx` only
affects new processes.

**Easier alternative, with a trade-off:** double-click the flow tab → **Environment**
tab → add `TELEGRAM_BOT_TOKEN`. No restart needed. But this value is stored inside
`flows.json` and **is included when you export the flow**, so don't use it if you plan
to share or publish your flow.

If the token is missing, the flow logs a warning and skips the notification — logging
still works.

---

## 10. Google Sheets (optional)

Skip this if the CSV is enough. If the variable below is not set, this branch quietly
does nothing and everything else is unaffected.

1. Create a Google Sheet, name the first tab **Attendance**.
2. Row 1: `Date | Time | StudentID | Name | Class | UID | Device`
3. **Extensions → Apps Script**, delete the sample, paste `google-apps-script.gs`, save.
4. **Deploy → New deployment → Web app**
   - Execute as: **Me**
   - Who has access: **Anyone with the link**
5. Authorise, copy the `/exec` URL.
6. Set it the same way as the Telegram token:

```bash
Environment="SHEETS_WEBAPP_URL=https://script.google.com/macros/s/..../exec"
```

⚠️ "Anyone with the link" means anyone who learns that URL can append rows. Set
`SHARED_SECRET` in the script (instructions are in its header comment) if this matters,
and never commit the URL to a public repo.

---

## 11. Test with no hardware

This is the payoff for doing the backend first.

Open the debug sidebar (🐞 icon, top right), then in **section 6** click the button on
the left of **Simulate tap** (edit its UID to `B96DF306`).

Expected, all within a second:

| Where | What you should see |
|---|---|
| `Match student` node | Green status: `B96DF306 -> ok` |
| `attendance.csv` | A new row appended |
| Debug sidebar | `telegram result` showing `{"ok":true,...}` |
| The parent's phone | *"Ali Bin Ahmad (S2026-001) has arrived at school at..."* |

Watch the ACK going back to the reader:

```bash
mosquitto_sub -h 192.168.137.1 -t 'school/attendance/#' -v
```

You should see:

```
school/attendance/ack {"status":"ok","uid":"A1B2C3D4","name":"Ali Bin Ahmad","line1":"Welcome!","line2":"Ali Bin Ahmad"}
```

**Now test the other two paths:**

- **Click the same inject again** → `duplicate`, LCD line `Already Tapped`, and
  **no second Telegram message**. That is the 5-minute cooldown doing its job.
- **Change the UID** in the inject node to `DEADBEEF` and deploy → `unknown`, a row in
  `unknown_uids.csv`, and `Unknown Card` on the ACK.

If all three behave, your backend is finished.

---

## 12. Connect the ESP32

In `firmware/secrets.h`:

```cpp
#define MQTT_HOST "192.168.137.1"        // already set for you
#define MQTT_PORT 1883
#define MQTT_USER ""                     // fill in if you used password Option B
#define MQTT_PASS ""
```

This is already done — `secrets.h` ships configured for your hotspot.

Upload, then open Serial Monitor at 115200. You want:

```
[WiFi] connecting to ...
[MQTT] connecting as esp32-gate-01-...
[MQTT] connected
```

Tap a card. The serial log shows `[TX] {"device":"gate-01","uid":"...","timestamp":"..."}`
and the same row appears in your CSV.

### MQTT topics used

| Topic | Direction | Payload |
|---|---|---|
| `school/attendance/scan` | ESP32 → Node-RED | `{"device":"gate-01","uid":"A1B2C3D4","timestamp":"2026-09-18T07:58:12+08:00"}` |
| `school/attendance/ack` | Node-RED → ESP32 | `{"status":"ok","uid":"...","name":"...","line1":"Welcome!","line2":"Ali Bin Ahmad"}` |
| `school/attendance/status` | ESP32 → broker | `{"device":"gate-01","online":true}` — retained, with a last-will so it flips to `false` if the reader dies |

`status` is genuinely useful: subscribe to it and you will know within 30 seconds if a
gate reader has lost power.

### HTTP instead of MQTT

The flow also accepts `POST http://192.168.137.1:1880/attendance` with the same JSON body,
and replies with the ACK JSON. Handy for testing from a laptop:

```bash
curl -X POST http://192.168.137.1:1880/attendance \
     -H "Content-Type: application/json" \
     -d '{"device":"laptop","uid":"B96DF306"}'
```

---

## 13. Day-to-day operation

### Enrolling a new student

1. Tap the new card on the reader. The LCD says `Unknown Card`.
2. Open `attendance/unknown_uids.csv` — the UID is on the last line.
3. Add a block to `students.json` with that UID as the key.
4. In Node-RED, click the **Load student DB** inject button (section 1).
5. Tap again → `Welcome!`

**No restart, no redeploy.** That inject button is the reload.

### Reading the log

`attendance.csv` opens directly in Excel, Google Sheets, or LibreOffice. Name fields are
quote-escaped, so a name like `Tan, Wei Ming` stays in one column.

### Changing the duplicate window

Default is 5 minutes. In the **Normalise + dedupe** function node:

```js
const COOLDOWN_MS = 5 * 60 * 1000;
```

Make it `4 * 60 * 60 * 1000` for "once per half-day" behaviour.

### Backing up

Three files are all that matter: `students.json`, `attendance/*.csv`, and your exported
flow. Copy them somewhere weekly.

---

## 14. Make it survive a reboot

### Raspberry Pi / Linux

Already done if you used the install script:

```bash
sudo systemctl enable nodered.service
sudo systemctl enable mosquitto
```

Check after a reboot with `sudo systemctl status nodered`.

### Windows

Mosquitto already installs as a service. Node-RED does not — running it from a Command
Prompt means it dies when the window closes or the user logs out. Install it as a
service with PM2:

```powershell
npm install -g pm2 pm2-windows-startup
pm2-startup install
pm2 start "C:\Users\<you>\AppData\Roaming\npm\node-red.cmd" --name node-red
pm2 save
```

Reboot and confirm the editor comes back on its own.

---

## 15. Before it goes live

You are about to handle children's names, arrival times, and parents' contact details.
Worth thirty minutes:

- **Password the broker.** Use Option B in section 4. An open broker on a school network
  lets anyone publish fake attendance records.
- **Password the Node-RED editor.** Anyone who reaches `:1880` can currently read your
  whole student database and edit the flow. Generate a hash:
  ```bash
  node-red admin hash-pw
  ```
  then uncomment and fill in `adminAuth` in `settings.js`, and restart.
- **Don't expose either port to the internet.** If you need remote access, use a VPN or
  Tailscale, not port forwarding.
- **Tell parents what you collect** and get their consent — the Telegram opt-in gives
  you a natural moment to do that.
- **Keep `students.json` out of any public repo.** It is a list of real children and
  their parents' contact details.

---

## 16. Troubleshooting

### MQTT

| Symptom | Cause and fix |
|---|---|
| Node-RED MQTT node stuck on **connecting** | Wrong IP/port, or Mosquitto is not running. `sudo systemctl status mosquitto`. |
| Node-RED connects on `localhost` but ESP32 cannot | Either the Mosquitto 2.x default (`listener 1883 0.0.0.0` missing) or the Windows Firewall rule on the hotspot adapter. Both are in section 4. |
| Everything worked, now nothing does | Windows Mobile Hotspot switched itself off — it does this on sleep and sometimes on idle. Turn it back on. |
| ESP32 serial shows `[MQTT] failed, state=-2` | Cannot reach the broker at all: wrong IP, firewall, or different subnet. |
| ESP32 shows `state=5` | Bad username/password. |
| ESP32 shows `state=-4` / keeps dropping | Two devices sharing a client ID, or weak Wi-Fi. Each reader needs a unique `DEVICE_ID`. |

### Flow

| Symptom | Cause and fix |
|---|---|
| Every tap comes back `unknown` | `students.json` did not load. Click **Load student DB** and watch the debug sidebar. Check the file path (section 7) and that the JSON is valid. |
| Nothing at all happens on a tap | Topic mismatch. The ESP32 publishes to `school/attendance/scan` — confirm the MQTT in node listens to exactly that. |
| No CSV file appears anywhere | Relative path resolved somewhere unexpected. Use absolute paths — section 7. |
| CSV has rows but no column titles | You skipped creating the header line in section 5. |
| `Load student DB` shows a red error | Invalid JSON. Paste `students.json` into jsonlint.com — it is usually a trailing comma. |

### Telegram

| Symptom | Cause and fix |
|---|---|
| Debug shows `{"ok":false,"error_code":401}` | Bad token, or the env var never reached Node-RED. Add a debug node after `Build Telegram message` to check. |
| `{"ok":false,"error_code":400,"description":"chat not found"}` | The parent has not pressed **Start** on your bot yet, or the chat ID is wrong. |
| Warning: `TELEGRAM_BOT_TOKEN is not set` | The environment variable is not visible to the Node-RED process. On Windows, restart the terminal after `setx`. On systemd, `daemon-reload` and restart. |
| Nothing happens, no error | The student has no `parentChatId` in `students.json`. Check `node.warn` output in the debug sidebar. |

### Still stuck?

Add a `debug` node set to **complete msg object** after `Match student` and deploy. It
shows you `msg.scan`, `msg.student` and `msg.result` for every tap, which narrows almost
any problem down to one node.

The **flow errors** catch node in section 5 already logs uncaught errors to the debug
sidebar and the console — check there first.
