/**************************************************************************************
 *  Smart RFID Student Attendance & Parent Notification System
 *  -----------------------------------------------------------------------------------
 *  Board    : ESP32 dev board + expansion board (incl. Cytron Maker ESP32)
 *  Reader   : Cytron RFID-RC522 kit, SPI (MFRC522v2 lib)  --  3.3V ONLY, NEVER 5V
 *  Display  : Cytron DS-LCD-162A-I2C, 16x2 @ 0x27  (SDA = GPIO21, SCL = GPIO22)
 *  Buzzer   : Onboard passive piezo on GPIO26 (mute switch must be ON)
 *  Backend  : Node-RED, reached over MQTT
 *
 *  DESIGN RULES FOLLOWED
 *   - There is NO delay() anywhere in loop(). Every timer uses millis().
 *   - Wi-Fi and MQTT each reconnect on their own schedule; a dead network never
 *     stops the reader from accepting a card.
 *   - The LCD is only rewritten when the text actually changes (saves I2C traffic).
 *   - Credentials live in secrets.h so this sketch can be shared safely.
 *
 *  LIBRARIES TO INSTALL (Arduino IDE -> Tools -> Manage Libraries)
 *   1. MFRC522v2            by GithubCommunity      -- talks to the RC522 reader
 *      >> search the Library Manager for "MFRC522v2", NOT "MFRC522". The two are
 *         different libraries with incompatible APIs; the old one will not compile.
 *   2. LiquidCrystal I2C    by Frank de Brabander   -- 16x2 LCD over I2C
 *   3. PubSubClient         by Nick O'Leary         -- MQTT client
 *   4. ArduinoJson (v7.x)   by Benoit Blanchon      -- builds/parses the JSON payload
 *   WiFi.h, SPI.h, Wire.h and time.h ship with the ESP32 board package.
 **************************************************************************************/

#include <WiFi.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

/* ===================================================================================
 *  1.  PIN MAP  -  standard ESP32 GPIOs; none clash with flash or input-only pins
 * =================================================================================== */
#define PIN_RC522_SCK    18      // SPI clock    (VSPI default)
#define PIN_RC522_MISO   19      // SPI data in  (VSPI default)
#define PIN_RC522_MOSI   23      // SPI data out (VSPI default)
#define PIN_RC522_SS      5      // Chip select  (RC522 label: SDA / NSS)
// RST -> NOT CONNECTED. The MFRC522v2 SPI driver has no reset-pin parameter; it
// resets the chip in software over SPI. The RC522's RST pin has an internal pull-up,
// so leaving it unwired is correct. Do NOT wire it to GPIO21 as some tutorials show
// -- GPIO21 is this project's LCD SDA line.

#define PIN_I2C_SDA      21      // LCD SDA (Maker Port SDA on a Maker ESP32)
#define PIN_I2C_SCL      22      // LCD SCL (Maker Port SCL on a Maker ESP32)

#define USE_INTERNAL_PULLUPS 0   // 1 only if you desoldered the LCD backpack pull-ups

// Onboard PASSIVE piezo of the Maker ESP32. Remember the hardware MUTE SWITCH:
// if it is off you hear nothing, whatever the code does. See BuzzerTest/ first.
#define PIN_BUZZER       26      // 26 = onboard piezo | 25 = external active buzzer
#define BUZZER_IS_ACTIVE  0      // 0 = passive piezo, driven with tone()
                                 // 1 = active buzzer, driven with plain HIGH/LOW
                                 // Each result pattern carries its own pitch (see
                                 // section 5). Run BuzzerTest option 6 to find where
                                 // your piezo is loudest and shift them if you like.

/* ===================================================================================
 *  2.  IDENTITY, TOPICS AND TIMING CONSTANTS
 * =================================================================================== */
#define DEVICE_ID        "gate-01"                  // which door this reader guards
#define TOPIC_SCAN       "school/attendance/scan"   // ESP32    -> Node-RED
#define TOPIC_ACK        "school/attendance/ack"    // Node-RED -> ESP32
#define TOPIC_STATUS     "school/attendance/status" // online / offline (retained)

const uint32_t RFID_POLL_MS      =   80;   // how often we ask "is a card there?"
const uint32_t SAME_CARD_LOCK_MS = 3000;   // ignore the same UID for 3 s (debounce)
const uint32_t LCD_HOLD_MS       = 2500;   // how long a result stays on screen
const uint32_t WIFI_RETRY_MS     = 10000;  // Wi-Fi reconnect attempt interval
const uint32_t MQTT_RETRY_MS     =  5000;  // MQTT reconnect attempt interval
const long     NTP_GMT_OFFSET_S  = 8 * 3600; // UTC+8 (Malaysia). Change for your country.

/* ===================================================================================
 *  3.  GLOBAL OBJECTS
 * =================================================================================== */
// MFRC522v2 builds the reader in three steps: a chip-select pin object, a bus
// driver that owns it, then the reader that talks through the driver.
MFRC522DriverPinSimple ssPin(PIN_RC522_SS);
MFRC522DriverSPI       driver{ssPin};
MFRC522                rfid{driver};

LiquidCrystal_I2C lcd(0x27, 16, 2);          // change to 0x3F if your backpack differs
WiFiClient        net;
PubSubClient      mqtt(net);

/* ===================================================================================
 *  4.  NON-BLOCKING STATE  -  every "timer" in this sketch is a millis() stamp
 * =================================================================================== */
uint32_t tRfidPoll   = 0;
uint32_t tWifiRetry  = 0;
uint32_t tMqttRetry  = 0;
uint32_t tLcdRelease = 0;      // 0 = idle screen, otherwise "revert at this millis()"

String   lastUid     = "";     // last UID seen, for the debounce window
uint32_t tLastUid    = 0;

String   lcdLine1    = "";     // what is currently ON the glass
String   lcdLine2    = "";

// --- buzzer note sequencer (same design as BuzzerTest / HardwareTest) -------------
struct Note {
  uint16_t freq;     // Hz, or 0 for a rest
  uint16_t ms;
};

const Note *seqData    = nullptr;
uint8_t     seqLen     = 0;
uint8_t     seqIdx     = 0;
uint32_t    tNextNote  = 0;
bool        seqRunning = false;


/* ===================================================================================
 *  5.  BUZZER  -  never blocks; loop() drives the pattern forward
 * =================================================================================== */
void toneOn(uint16_t freq) {
#if BUZZER_IS_ACTIVE
  (void)freq;                          // an active buzzer has one pitch of its own
  digitalWrite(PIN_BUZZER, HIGH);
#else
  tone(PIN_BUZZER, freq);
#endif
}

void toneOff() {
#if BUZZER_IS_ACTIVE
  digitalWrite(PIN_BUZZER, LOW);
#else
  noTone(PIN_BUZZER);
#endif
}

// Start a pattern. Returns immediately -- the sound happens in serviceTone().
void playPattern(const Note *notes, uint8_t len) {
  seqData    = notes;
  seqLen     = len;
  seqIdx     = 0;
  tNextNote  = millis();
  seqRunning = true;
}

void serviceTone() {
  if (!seqRunning) return;
  if ((int32_t)(millis() - tNextNote) < 0) return;

  if (seqIdx >= seqLen) {
    toneOff();
    seqRunning = false;
    return;
  }

  const Note &n = seqData[seqIdx];
  if (n.freq == 0) toneOff();
  else             toneOn(n.freq);

  tNextNote = millis() + n.ms;
  seqIdx++;
}

/* --- one sound per attendance result, so staff can hear what happened ---------
   Node-RED sets the status; the firmware only picks the pattern.               */
const Note P_BOOT[]   = { {1800,  90}, {0, 50}, {2600, 130} };            // powered up
const Note P_TAP[]    = { {2500,  60} };                                  // card seen
const Note P_ONTIME[] = { {2000,  70}, {0, 40}, {2800,  90} };            // welcome, rising
const Note P_LATE[]   = { {2600,  70}, {0, 60}, {2000,  70}, {0, 60},
                          {1500, 220} };                                  // falling = late
const Note P_OUT[]    = { {2200, 110}, {0, 50}, {1600, 200} };            // goodbye
const Note P_DUP[]    = { {1200, 300} };                                  // already tapped
const Note P_ERR[]    = { { 600, 180}, {0, 90}, { 600, 180}, {0, 90},
                          { 600, 260} };                                  // unknown card

#define LEN(a) (sizeof(a) / sizeof(a[0]))

/* ===================================================================================
 *  6.  LCD HELPERS  -  only touch the I2C bus when the text really changed
 * =================================================================================== */
String pad16(const String &s) {
  String t = s.substring(0, 16);
  while (t.length() < 16) t += ' ';
  return t;
}

void lcdShow(const String &l1, const String &l2) {
  String a = pad16(l1), b = pad16(l2);
  if (a == lcdLine1 && b == lcdLine2) return;   // nothing changed, skip the write
  lcdLine1 = a; lcdLine2 = b;
  lcd.setCursor(0, 0); lcd.print(a);
  lcd.setCursor(0, 1); lcd.print(b);
}

// Show a result for LCD_HOLD_MS, then fall back to the idle screen automatically.
void lcdFlash(const String &l1, const String &l2) {
  lcdShow(l1, l2);
  tLcdRelease = millis() + LCD_HOLD_MS;
}

// The idle screen doubles as a status indicator.
void lcdIdle() {
  String s;
  if (WiFi.status() != WL_CONNECTED) s = "WiFi...";
  else if (!mqtt.connected())        s = "Server...";
  else                               s = WiFi.localIP().toString();
  lcdShow("Tap Your Card...", s);
}

void serviceLcd() {
  if (tLcdRelease == 0) { lcdIdle(); return; }
  if ((int32_t)(millis() - tLcdRelease) >= 0) {
    tLcdRelease = 0;
    lcdIdle();
  }
}

// Runs once at boot. If the LCD stays blank, check this output first:
//   "0x27" found  -> wiring is fine, it is a contrast problem (turn the blue pot)
//   "0x3F" found  -> change the LiquidCrystal_I2C(...) address at the top of this file
//   nothing found -> SDA/SCL swapped, no power to the backpack, or a loose wire
void i2cScan() {
  Serial.println("[I2C] scanning...");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C]   device at 0x%02X\n", addr);
      found++;
    }
  }
  if (!found) Serial.println("[I2C]   nothing found -- check SDA/SCL and power");
}

/* ===================================================================================
 *  7.  TIME  -  ISO-8601 stamp for the JSON payload
 * =================================================================================== */
// Returns "2026-09-17T08:12:33+08:00", or "" if NTP has not synced yet.
// If it is empty, Node-RED stamps the record with its own clock instead.
String isoTimestamp() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";          // clock clearly not set yet
  struct tm tmNow;
  localtime_r(&now, &tmNow);

  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tmNow);

  // append the fixed UTC offset that matches NTP_GMT_OFFSET_S
  char out[48];
  long offMin = NTP_GMT_OFFSET_S / 60;
  snprintf(out, sizeof(out), "%s%+03ld:%02ld", stamp, offMin / 60, labs(offMin) % 60);
  return String(out);
}

/* ===================================================================================
 *  8.  Wi-Fi  -  kicked off in setup(), healed in loop(), never blocks
 * =================================================================================== */
void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  Serial.printf("[WiFi] connecting to %s ...\n", ssid);
}

void serviceWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - tWifiRetry < WIFI_RETRY_MS) return;
  tWifiRetry = millis();
  Serial.println("[WiFi] down -- retrying");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
}

/* ===================================================================================
 *  9.  MQTT
 * =================================================================================== */
// Node-RED replies here so the LCD can confirm the tap was accepted.
// Expected payload: {"status":"ok","name":"...","line1":"Welcome!","line2":"Ali B."}
void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) {
    Serial.println("[MQTT] ack was not valid JSON");
    return;
  }
  const char *l1 = doc["line1"]  | "Recorded";
  const char *l2 = doc["line2"]  | "";
  const char *st = doc["status"] | "ok";

  lcdFlash(l1, l2);

  // Node-RED decides what happened; we just play the matching sound.
  //   in_ontime  arrived within the grace period
  //   in_late    arrived after it -- distinctly FALLING so it is obvious
  //   out        went home
  //   duplicate  tapped again, nothing recorded
  //   unknown    card is not on the roster
  if      (strcmp(st, "in_ontime") == 0) playPattern(P_ONTIME, LEN(P_ONTIME));
  else if (strcmp(st, "ok")        == 0) playPattern(P_ONTIME, LEN(P_ONTIME));
  else if (strcmp(st, "in_late")   == 0) playPattern(P_LATE,   LEN(P_LATE));
  else if (strcmp(st, "out")       == 0) playPattern(P_OUT,    LEN(P_OUT));
  else if (strcmp(st, "duplicate") == 0) playPattern(P_DUP,    LEN(P_DUP));
  else                                   playPattern(P_ERR,    LEN(P_ERR));
}

void serviceMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) { mqtt.loop(); return; }

  if (millis() - tMqttRetry < MQTT_RETRY_MS) return;
  tMqttRetry = millis();

  String clientId = String("esp32-") + DEVICE_ID + "-" +
                    String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] connecting as %s\n", clientId.c_str());

  // NOTE: connect() is the ONE call that can briefly block. setSocketTimeout(2)
  // in setup() caps that at ~2 s, and we only attempt it once every MQTT_RETRY_MS,
  // so card reading stays responsive even when the broker is switched off.
  const char *willMsg = "{\"device\":\"" DEVICE_ID "\",\"online\":false}";
  bool ok = (strlen(MQTT_USER) > 0)
              ? mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                             TOPIC_STATUS, 1, true, willMsg)
              : mqtt.connect(clientId.c_str(), TOPIC_STATUS, 1, true, willMsg);

  if (ok) {
    Serial.println("[MQTT] connected");
    mqtt.publish(TOPIC_STATUS, "{\"device\":\"" DEVICE_ID "\",\"online\":true}", true);
    mqtt.subscribe(TOPIC_ACK, 1);
  } else {
    Serial.printf("[MQTT] failed, state=%d (see PubSubClient.h for codes)\n", mqtt.state());
  }
}

// Build and send {"device":..,"uid":..,"timestamp":..} for one tap.
void publishScan(const String &uid) {
  JsonDocument doc;
  doc["device"]    = DEVICE_ID;
  doc["uid"]       = uid;
  doc["timestamp"] = isoTimestamp();   // may be "" -> Node-RED fills it in

  char json[192];
  size_t n = serializeJson(doc, json, sizeof(json));
  Serial.printf("[TX] %s\n", json);

  if (mqtt.connected()) {
    mqtt.publish(TOPIC_SCAN, (const uint8_t *)json, n, false);
  } else {
    // Offline: the tap still beeps and shows on the LCD, but nothing is logged.
    lcdFlash("Offline!", "Not recorded");
  }
}

/* ===================================================================================
 * 10.  RFID
 * =================================================================================== */
String uidToHex(const MFRC522::Uid &uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += '0';
    s += String(uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void serviceRfid() {
  if (millis() - tRfidPoll < RFID_POLL_MS) return;
  tRfidPoll = millis();

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  String uid = uidToHex(rfid.uid);

  // Close the session with this card straight away so the reader is free again.
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // Debounce: a card held near the antenna re-triggers many times per second.
  if (uid == lastUid && millis() - tLastUid < SAME_CARD_LOCK_MS) return;
  lastUid  = uid;
  tLastUid = millis();

  Serial.printf("[RFID] UID %s\n", uid.c_str());
  playPattern(P_TAP, LEN(P_TAP));      // instant feedback, before the network round-trip
  lcdFlash("Card Tapped!", uid);
  publishScan(uid);
  // The final verdict ("Welcome, Ali") arrives asynchronously in onMqttMessage().
}

/* ===================================================================================
 * 11.  SETUP
 * =================================================================================== */
void setup() {
  Serial.begin(115200);
  delay(100);                          // one-off boot settle, outside loop() -- allowed
  Serial.println("\n== Smart RFID Attendance :: " DEVICE_ID " ==");

  // --- buzzer ---
  pinMode(PIN_BUZZER, OUTPUT);
  toneOff();

  // --- LCD ---
  // The Cytron DS-LCD-162A-I2C backpack carries its own ~4.7k pull-ups to ITS Vcc.
  // If you powered it from 5 V, the bus idles at 5 V, which is above the ESP32's
  // 3.6 V absolute maximum -- see README section 2.6(b) before wiring.
  // If you removed those resistors (README option 3), set USE_INTERNAL_PULLUPS to 1.
#if USE_INTERNAL_PULLUPS
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
#endif
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);               // 100 kHz is kind to long jumper wires
  i2cScan();                           // prints every address found -- expect 0x27
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdShow("RFID Attendance", "Starting up...");

  // --- RC522 over VSPI ---
  // Start SPI with our pins first. They are the ESP32 defaults, and SPIClass::begin()
  // returns early if the bus is already up, so the driver's own begin() is a no-op.
  SPI.begin(PIN_RC522_SCK, PIN_RC522_MISO, PIN_RC522_MOSI, PIN_RC522_SS);
  rfid.PCD_Init();
  MFRC522Debug::PCD_DumpVersionToSerial(rfid, Serial);

  // The RC522 often ignores its very first version read after power-up -- it is still
  // coming out of reset while we are already probing it. Retry before reporting a
  // fault, otherwise a perfectly good reader looks broken at every boot.
  byte rcVer = 0;
  for (uint8_t i = 0; i < 5; i++) {
    rcVer = (byte)rfid.PCD_GetVersion();
    if (rcVer != 0x00 && rcVer != 0xFF) break;
    delay(50);                         // setup() only -- loop() stays non-blocking
  }
  if (rcVer == 0x00 || rcVer == 0xFF) {
    Serial.println("[RC522] version read got no answer -- carrying on anyway.");
    Serial.println("[RC522] If cards still scan, ignore it. Tying RST to 3V3 fixes it.");
  } else {
    Serial.printf("[RC522] version = 0x%02X  OK\n", rcVer);
  }

  // --- network ---
  wifiBegin();
  configTime(NTP_GMT_OFFSET_S, 0, "pool.ntp.org", "time.google.com");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);             // default 256 is tight once names are included
  mqtt.setSocketTimeout(2);            // caps how long a failed connect() can stall us
  mqtt.setKeepAlive(30);

  playPattern(P_BOOT, LEN(P_BOOT));    // two-note chirp = firmware is alive
}

/* ===================================================================================
 * 12.  LOOP  -  five independent services, none of them blocking
 * =================================================================================== */
void loop() {
  serviceWifi();     // heal Wi-Fi if it dropped
  serviceMqtt();     // heal MQTT, pump incoming acks
  serviceRfid();     // poll for a card
  serviceTone();     // advance the beep pattern
  serviceLcd();      // release the result screen when its time is up
}
