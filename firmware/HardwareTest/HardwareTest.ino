/**************************************************************************************
 *  HardwareTest.ino  --  RC522 + I2C LCD + buzzer, with NO network at all
 *  -----------------------------------------------------------------------------------
 *  Board   : Cytron Maker ESP32
 *  Reader  : Cytron RFID-RC522 kit (SPI, 3.3V ONLY)
 *  Display : Cytron DS-LCD-162A-I2C, 16x2 @ 0x27
 *  Buzzer  : onboard passive piezo on GPIO26 (MUTE SWITCH must be ON)
 *
 *  >>> THIS VERSION USES THE **MFRC522v2** LIBRARY <<<
 *  The same one as the Random Nerd Tutorials ESP32 guide. It is NOT compatible with
 *  the older "MFRC522" library -- the API is completely different, so code written
 *  for one will not compile against the other. See "LIBRARIES" below.
 *
 *  WHAT THIS SKETCH IS FOR
 *  Prove the hardware works before adding Wi-Fi and MQTT. If something breaks after
 *  you add networking, come back here: if this sketch still works, the fault is in the
 *  network layer, not the wiring. That single fact saves hours.
 *
 *  It gives you the COMPLETE gate experience offline -- known card, unknown card and
 *  repeat tap all behave exactly as they will in the finished system, because the
 *  student list below stands in for the Node-RED lookup.
 *
 *  WHAT IT DELIBERATELY DOES NOT DO
 *    - No Wi-Fi, no MQTT, no ArduinoJson, no secrets.h. Nothing to configure.
 *    - Nothing is logged anywhere permanent. Power off and the counters reset.
 *
 *  LIBRARIES (Arduino IDE -> Tools -> Manage Libraries)
 *    1. MFRC522v2          by GithubCommunity   <-- search "MFRC522v2", NOT "MFRC522"
 *    2. LiquidCrystal I2C  by Frank de Brabander
 *  SPI.h and Wire.h ship with the ESP32 board package.
 *
 *  Board: "ESP32 Dev Module".  Serial Monitor: 115200 baud.
 *
 *  Still non-blocking: no delay() in loop(), every timer is a millis() stamp. Same
 *  architecture as the full project, so everything here transfers across unchanged.
 **************************************************************************************/

#include <SPI.h>
#include <Wire.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include <LiquidCrystal_I2C.h>

/* ===================================================================================
 *  1.  PIN MAP
 *
 *  !! RST IS DELIBERATELY NOT WIRED !!
 *  The MFRC522v2 SPI driver has no reset-pin parameter at all -- it resets the chip
 *  over SPI in software. The RC522's RST pin has an internal pull-up, so leaving it
 *  unconnected is correct and is exactly what the Random Nerd Tutorials sketch does
 *  (it wires the pin but never references it in code).
 *
 *  DO NOT follow that tutorial's "RST -> GPIO21" wiring in THIS project: GPIO21 is
 *  the LCD's SDA line here. Tying the reader's reset pin onto the I2C data line
 *  breaks the display AND can hold the reader in reset. Leave RST unconnected, or
 *  tie it to 3V3 if you prefer it pinned high.
 * =================================================================================== */
#define PIN_RC522_SCK    18      // SPI clock    (ESP32 VSPI default)
#define PIN_RC522_MISO   19      // SPI data in  (ESP32 VSPI default)
#define PIN_RC522_MOSI   23      // SPI data out (ESP32 VSPI default)
#define PIN_RC522_SS      5      // Chip select  (module labels it SDA or NSS)
                                 // RST -> not connected

#define PIN_I2C_SDA      21      // LCD SDA
#define PIN_I2C_SCL      22      // LCD SCL

#define PIN_BUZZER       26      // 26 = onboard piezo | 25 = external active buzzer
#define BUZZER_IS_ACTIVE  0      // 0 = passive piezo (tone) | 1 = active (HIGH/LOW)

#define LCD_ADDRESS    0x27      // the boot-time I2C scan will tell you if it differs

/* ===================================================================================
 *  2.  THE LOCAL "DATABASE"
 *  Stands in for the Node-RED lookup so you can test the full behaviour offline.
 *  Tap a card, read the UID from Serial Monitor, paste it in here, re-upload.
 * =================================================================================== */
struct Student {
  const char *uid;     // UPPERCASE hex, no spaces, exactly as Serial prints it
  const char *name;    // keep to 16 characters -- that is all the LCD can show
};

const Student STUDENTS[] = {
  // Your four real cards, UIDs taken from your own serial log.
  // Replace the names with the real students -- 16 characters max.
  { "B96DF306", "Student 01" },
  { "97513A25", "Student 02" },
  { "8EEB2907", "Student 03" },
  { "295E5514", "Student 04" },
};
const uint8_t STUDENT_COUNT = sizeof(STUDENTS) / sizeof(STUDENTS[0]);

// Short on purpose so you can actually test the repeat-tap path without waiting.
// The real system uses 5 minutes, enforced in Node-RED.
const uint32_t DUPLICATE_WINDOW_MS = 10000;

/* ===================================================================================
 *  3.  TIMING
 * =================================================================================== */
const uint32_t RFID_POLL_MS = 80;      // how often we ask "is a card there?"
const uint32_t LCD_HOLD_MS  = 2500;    // how long a result stays on screen

/* ===================================================================================
 *  4.  OBJECTS AND STATE
 *
 *  Note the three-step construction -- this is the big visible difference from the
 *  old library. Instead of MFRC522 rfid(ssPin, rstPin), v2 builds a chip-select pin
 *  object, wraps it in a bus driver, then hands the driver to the reader.
 * =================================================================================== */
MFRC522DriverPinSimple ssPin(PIN_RC522_SS);   // chip-select pin
MFRC522DriverSPI       driver{ssPin};         // SPI bus driver (uses the SPI object)
MFRC522                rfid{driver};          // the reader itself

LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);

uint32_t tRfidPoll   = 0;
uint32_t tLcdRelease = 0;
String   lcdLine1    = "";
String   lcdLine2    = "";

String   lastUid     = "";
uint32_t tLastUid    = 0;

uint16_t countTotal   = 0;             // every accepted tap
uint16_t countKnown   = 0;
uint16_t countUnknown = 0;
bool     readerOk     = false;

/* ===================================================================================
 *  5.  BUZZER  -  non-blocking note sequencer
 *
 *  Each pattern below carries its own pitch, which is the whole point of a passive
 *  piezo. Set BUZZER_IS_ACTIVE to 1 and the pitches are ignored -- an active buzzer
 *  has one note of its own, so the patterns collapse to rhythm only. They still
 *  sound distinct from each other, just less pleasant.
 * =================================================================================== */
struct Note {
  uint16_t freq;     // Hz, or 0 for a rest
  uint16_t ms;
};

const Note *seqData    = nullptr;
uint8_t     seqLen     = 0;
uint8_t     seqIdx     = 0;
uint32_t    tNextNote  = 0;
bool        seqRunning = false;

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

// --- the four sounds of the finished system ---------------------------------------
const Note P_BOOT[] = { {1800,  90}, {0, 50}, {2600, 130} };
const Note P_TAP[]  = { {2500,  60} };
const Note P_OK[]   = { {2000,  70}, {0, 40}, {2800,  90} };
const Note P_DUP[]  = { {1200, 300} };
const Note P_ERR[]  = { { 600, 180}, {0, 90}, { 600, 180}, {0, 90}, {600, 260} };

// All four in a row, for the 'b' serial command.
const Note P_DEMO[] = {
  {2500,  60}, {0, 400},                                     // tap
  {2000,  70}, {0,  40}, {2800,  90}, {0, 400},              // accepted
  {1200, 300}, {0, 400},                                     // duplicate
  { 600, 180}, {0,  90}, { 600, 180}, {0,  90}, {600, 260}   // unknown
};

#define LEN(a) (sizeof(a) / sizeof(a[0]))

/* ===================================================================================
 *  6.  LCD  -  only write when the text actually changed
 * =================================================================================== */
String pad16(const String &s) {
  String t = s.substring(0, 16);
  while (t.length() < 16) t += ' ';
  return t;
}

void lcdShow(const String &l1, const String &l2) {
  String a = pad16(l1), b = pad16(l2);
  if (a == lcdLine1 && b == lcdLine2) return;
  lcdLine1 = a; lcdLine2 = b;
  lcd.setCursor(0, 0); lcd.print(a);
  lcd.setCursor(0, 1); lcd.print(b);
}

void lcdFlash(const String &l1, const String &l2) {
  lcdShow(l1, l2);
  tLcdRelease = millis() + LCD_HOLD_MS;
}

// Idle screen doubles as a live scoreboard while you test.
void lcdIdle() {
  // Not a hard fault: the boot probe failed, but a successful tap will clear it.
  if (!readerOk) { lcdShow("Reader slow?", "Tap to confirm"); return; }
  lcdShow("Tap Your Card...", "OK:" + String(countKnown) + " Unk:" + String(countUnknown));
}

void serviceLcd() {
  if (tLcdRelease == 0) { lcdIdle(); return; }
  if ((int32_t)(millis() - tLcdRelease) >= 0) {
    tLcdRelease = 0;
    lcdIdle();
  }
}

/* ===================================================================================
 *  7.  DIAGNOSTICS  -  run once at boot, and on demand from the serial menu
 * =================================================================================== */
void i2cScan() {
  Serial.println("[I2C] scanning...");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C]   device at 0x%02X%s\n", addr,
                    addr == LCD_ADDRESS ? "  <- the LCD, as expected" : "");
      found++;
    }
  }
  if (!found) {
    Serial.println("[I2C]   NOTHING FOUND");
    Serial.println("[I2C]   -> SDA/SCL swapped, backpack unpowered, or a loose wire");
  }
}

// v2 gives us a proper version enum instead of a raw register read.
// Comparing raw byte values keeps this readable and avoids enum-scope surprises.
//
// WHY THE RETRY: the RC522 often does not answer its very first version read after
// power-up -- the chip is still coming out of reset while the ESP32 is already
// probing it. One failed read at boot means nothing; the reader usually works
// perfectly a moment later. Retrying removes that false alarm.
// (The delay here is bounded and only ever runs from setup() or the 'v' command,
//  never from the main loop.)
bool checkReader(uint8_t attempts) {
  byte v = 0;
  for (uint8_t i = 0; i < attempts; i++) {
    v = (byte)rfid.PCD_GetVersion();
    if (v != 0x00 && v != 0xFF) break;     // got a real answer, stop retrying
    if (i + 1 < attempts) delay(50);
  }
  Serial.printf("[RC522] version = 0x%02X  ", v);

  switch (v) {
    case 0x88: Serial.println("OK (FM17522 clone)");   return true;
    case 0x89: Serial.println("OK (FM17522E clone)");  return true;
    case 0xB2: Serial.println("OK (FM17522 clone)");   return true;
    case 0x90: Serial.println("OK (v0.0)");            return true;
    case 0x91: Serial.println("OK (v1.0)");            return true;
    case 0x92: Serial.println("OK (v2.0)");            return true;
    case 0x12: Serial.println("COUNTERFEIT chip");     return false;
    default:   Serial.println("BAD");                  break;
  }

  Serial.println("[RC522] 0x00 or 0xFF means the version read got no answer.");
  Serial.println("[RC522] If cards still scan fine below, IGNORE THIS -- the chip was");
  Serial.println("[RC522] just slow to wake. Tying RST to 3V3 makes it deterministic.");
  Serial.println("[RC522] If cards do NOT scan either, then check:");
  Serial.println("[RC522]   - a cold solder joint on the header (MISO is the usual one)");
  Serial.println("[RC522]   - MOSI/MISO swapped");
  Serial.println("[RC522]   - module powered from 5V instead of 3.3V");
  Serial.println("[RC522]   - wrong library: this sketch needs MFRC522v2");
  return false;
}

/* ===================================================================================
 *  8.  RFID
 *  The reading code itself is unchanged from the old library -- uid.size and
 *  uid.uidByte[] have the same names in v2, so this all transfers as-is.
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

// Returns the student's name, or nullptr if the UID is not in the table.
const char *findStudent(const String &uid) {
  for (uint8_t i = 0; i < STUDENT_COUNT; i++) {
    if (uid.equals(STUDENTS[i].uid)) return STUDENTS[i].name;
  }
  return nullptr;
}

void serviceRfid() {
  if (millis() - tRfidPoll < RFID_POLL_MS) return;
  tRfidPoll = millis();

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  String uid = uidToHex(rfid.uid);

  // A card just came back cleanly, so the reader is demonstrably working. Clear any
  // false READER FAULT left over from a slow boot-time version read.
  if (!readerOk) {
    readerOk = true;
    Serial.println("[RC522] card read OK -> clearing the boot-time fault warning");
  }

  // Release the card immediately so the reader is free again.
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // --- instant feedback, before any lookup ---
  playPattern(P_TAP, LEN(P_TAP));
  lcdFlash("Card Tapped!", uid);
  countTotal++;

  Serial.println();
  Serial.printf("[TAP #%u] UID %s  (%d bytes)\n", countTotal, uid.c_str(), rfid.uid.size);

  // --- repeat tap of the same card? ---
  bool duplicate = (uid == lastUid) && (millis() - tLastUid < DUPLICATE_WINDOW_MS);
  lastUid  = uid;
  tLastUid = millis();

  const char *name = findStudent(uid);

  if (duplicate) {
    Serial.println("   result : DUPLICATE (inside the repeat window)");
    lcdFlash("Already Tapped", name ? name : uid);
    playPattern(P_DUP, LEN(P_DUP));
    return;
  }

  if (name) {
    countKnown++;
    Serial.printf("   result : OK -> %s\n", name);
    // Exactly what the networked version would publish. Useful to eyeball now.
    Serial.printf("   would send: {\"device\":\"gate-01\",\"uid\":\"%s\"}\n", uid.c_str());
    lcdFlash("Welcome!", name);
    playPattern(P_OK, LEN(P_OK));
  } else {
    countUnknown++;
    Serial.println("   result : UNKNOWN card");
    Serial.println("   to enrol it, add this line to STUDENTS[] and re-upload:");
    Serial.printf("     { \"%s\", \"Student Name\" },\n", uid.c_str());
    lcdFlash("Unknown Card", uid);
    playPattern(P_ERR, LEN(P_ERR));
  }
}

/* ===================================================================================
 *  9.  SERIAL MENU
 * =================================================================================== */
void printMenu() {
  Serial.println();
  Serial.println("=== commands ===");
  Serial.println("  l = list the students in this sketch");
  Serial.println("  c = clear the repeat-tap memory");
  Serial.println("  i = re-run the I2C scan");
  Serial.println("  v = re-check the RC522");
  Serial.println("  b = play the four result sounds");
  Serial.println("  r = reset the counters");
  Serial.println("  m = this menu");
  Serial.println("Now tap a card.");
}

void handleKey(char c) {
  switch (c) {
    case 'l':
      Serial.printf("\n%u students known to this sketch:\n", STUDENT_COUNT);
      for (uint8_t i = 0; i < STUDENT_COUNT; i++)
        Serial.printf("   %-10s %s\n", STUDENTS[i].uid, STUDENTS[i].name);
      break;
    case 'c':
      lastUid = ""; tLastUid = 0;
      Serial.println("repeat-tap memory cleared - the next tap counts as new");
      break;
    case 'i': i2cScan(); break;
    case 'v': readerOk = checkReader(3); break;
    case 'b':
      Serial.println("playing: tap ... accepted ... duplicate ... unknown");
      playPattern(P_DEMO, LEN(P_DEMO));
      break;
    case 'r':
      countTotal = countKnown = countUnknown = 0;
      Serial.println("counters reset");
      break;
    case 'm': printMenu(); break;
    default: break;                        // ignore newlines and stray characters
  }
}

/* ===================================================================================
 * 10.  SETUP
 * =================================================================================== */
void setup() {
  Serial.begin(115200);
  delay(300);                              // one-off, so the banner is not cut off
  Serial.println("\n\n===== RFID + LCD + BUZZER hardware test (no network) =====");
  Serial.println("      library: MFRC522v2");

  // --- buzzer ---
  pinMode(PIN_BUZZER, OUTPUT);
  toneOff();

  // --- LCD ---
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  i2cScan();

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdShow("Hardware Test", "Starting up...");

  // --- RC522 ---
  // Start SPI with our pins FIRST. These happen to be the ESP32 defaults, and
  // SPIClass::begin() on ESP32 returns early if the bus is already up, so the
  // driver's own begin() inside PCD_Init() becomes a harmless no-op.
  SPI.begin(PIN_RC522_SCK, PIN_RC522_MISO, PIN_RC522_MOSI, PIN_RC522_SS);

  rfid.PCD_Init();
  MFRC522Debug::PCD_DumpVersionToSerial(rfid, Serial);   // human-readable line
  readerOk = checkReader(5);                             // retries before giving up
  if (!readerOk) {
    Serial.println("[RC522] carrying on anyway -- tap a card, it may well work.");
  }

  // --- you should hear this and see the screen change ---
  playPattern(P_BOOT, LEN(P_BOOT));

  Serial.println();
  Serial.println("If the LCD is blank but backlit -> turn the blue contrast pot.");
  Serial.println("If you hear nothing at all      -> check the board's MUTE SWITCH.");
  printMenu();
}

/* ===================================================================================
 * 11.  LOOP  -  three services, none of them blocking
 * =================================================================================== */
void loop() {
  while (Serial.available()) handleKey((char)Serial.read());

  serviceRfid();     // poll for a card
  serviceTone();     // advance the beep pattern
  serviceLcd();      // release the result screen when its time is up
}
