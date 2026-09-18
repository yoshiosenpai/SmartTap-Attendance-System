/**************************************************************************************
 *  Smart RFID Student Attendance & Parent Notification System
 *  -----------------------------------------------------------------------------------
 *  Board    : Cytron Maker ESP32 (ESP32-WROOM-32E-N8) + expansion board
 *  Reader   : Mifare RC522, SPI  --  3.3V ONLY, NEVER 5V
 *  Display  : I2C 16x2 LCD @ 0x27 on the Maker Port (SDA = GPIO21, SCL = GPIO22)
 *  Buzzer   : Active buzzer on GPIO25  (onboard passive piezo on GPIO26 = alternative)
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
 *   1. MFRC522              by GithubCommunity      -- talks to the RC522 reader
 *   2. LiquidCrystal I2C    by Frank de Brabander   -- 16x2 LCD over I2C
 *   3. PubSubClient         by Nick O'Leary         -- MQTT client
 *   4. ArduinoJson (v7.x)   by Benoit Blanchon      -- builds/parses the JSON payload
 *   WiFi.h, SPI.h, Wire.h and time.h ship with the ESP32 board package.
 **************************************************************************************/

#include <WiFi.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

/* ===================================================================================
 *  1.  PIN MAP  -  every pin here is confirmed safe on the Maker ESP32
 * =================================================================================== */
#define PIN_RC522_SCK    18      // SPI clock    (VSPI default)
#define PIN_RC522_MISO   19      // SPI data in  (VSPI default)
#define PIN_RC522_MOSI   23      // SPI data out (VSPI default)
#define PIN_RC522_SS      5      // Chip select  (RC522 label: SDA / NSS)
#define PIN_RC522_RST    27      // Reset

#define PIN_I2C_SDA      21      // Maker Port SDA -- LCD
#define PIN_I2C_SCL      22      // Maker Port SCL -- LCD

#define PIN_BUZZER       25      // External ACTIVE buzzer (+ leg)
#define BUZZER_IS_ACTIVE  1      // 1 = active buzzer (plain HIGH/LOW)
                                 // 0 = passive piezo (uses tone(); onboard one is GPIO26)
#define BUZZER_TONE_HZ 2500      // only used when BUZZER_IS_ACTIVE is 0

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
MFRC522           rfid(PIN_RC522_SS, PIN_RC522_RST);
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

// --- buzzer beep-pattern state machine -------------------------------------------
uint8_t  beepsLeft   = 0;      // how many beeps still owed
bool     beepOn      = false;
uint32_t tBeepNext   = 0;
uint16_t beepOnMs    = 80;
uint16_t beepOffMs   = 80;


/* ===================================================================================
 *  5.  BUZZER  -  never blocks; loop() drives the pattern forward
 * =================================================================================== */
void buzzerWrite(bool on) {
#if BUZZER_IS_ACTIVE
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
#else
  if (on) tone(PIN_BUZZER, BUZZER_TONE_HZ);
  else    noTone(PIN_BUZZER);
#endif
}

// Ask for N beeps. Returns immediately -- the sound happens in serviceBuzzer().
void beep(uint8_t times, uint16_t onMs = 80, uint16_t offMs = 80) {
  beepsLeft = times;
  beepOnMs  = onMs;
  beepOffMs = offMs;
  beepOn    = false;
  tBeepNext = millis();        // start on the very next loop pass
}

void serviceBuzzer() {
  if (beepsLeft == 0 && !beepOn) return;
  if ((int32_t)(millis() - tBeepNext) < 0) return;

  if (!beepOn) {               // time to start a beep
    buzzerWrite(true);
    beepOn    = true;
    tBeepNext = millis() + beepOnMs;
  } else {                     // time to end a beep
    buzzerWrite(false);
    beepOn    = false;
    if (beepsLeft > 0) beepsLeft--;
    tBeepNext = millis() + beepOffMs;
  }
}

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
  if      (strcmp(st, "ok")        == 0) beep(2, 70, 70);    // accepted = two short beeps
  else if (strcmp(st, "duplicate") == 0) beep(1, 300, 0);    // repeat   = one long beep
  else                                   beep(3, 250, 120);  // unknown  = three long beeps
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
  beep(1, 60, 0);                      // instant feedback, before the network round-trip
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
  buzzerWrite(false);

  // --- LCD on the Maker Port ---
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdShow("RFID Attendance", "Starting up...");

  // --- RC522 over VSPI ---
  SPI.begin(PIN_RC522_SCK, PIN_RC522_MISO, PIN_RC522_MOSI, PIN_RC522_SS);
  rfid.PCD_Init();
  rfid.PCD_DumpVersionToSerial();      // 0x92 / 0x91 = good, 0x00 or 0xFF = wiring fault

  // --- network ---
  wifiBegin();
  configTime(NTP_GMT_OFFSET_S, 0, "pool.ntp.org", "time.google.com");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);             // default 256 is tight once names are included
  mqtt.setSocketTimeout(2);            // caps how long a failed connect() can stall us
  mqtt.setKeepAlive(30);

  beep(1, 120, 0);                     // one chirp = firmware is alive
}

/* ===================================================================================
 * 12.  LOOP  -  five independent services, none of them blocking
 * =================================================================================== */
void loop() {
  serviceWifi();     // heal Wi-Fi if it dropped
  serviceMqtt();     // heal MQTT, pump incoming acks
  serviceRfid();     // poll for a card
  serviceBuzzer();   // advance the beep pattern
  serviceLcd();      // release the result screen when its time is up
}
