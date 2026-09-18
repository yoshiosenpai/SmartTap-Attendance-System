/**************************************************************************************
 *  BuzzerTest.ino  --  test the ONBOARD buzzer of the Cytron Maker ESP32
 *  -----------------------------------------------------------------------------------
 *  Board  : Cytron Maker ESP32   |   Buzzer: onboard PASSIVE piezo on GPIO26
 *
 *  >>> BEFORE YOU BLAME THE CODE <<<
 *  The Maker ESP32 has a hardware MUTE SWITCH next to the buzzer.
 *  If it is in the OFF/mute position you will hear absolutely nothing, no matter
 *  what the firmware does. This is the number one cause of "my buzzer is broken".
 *  Slide it ON first.
 *
 *  WHY tone() AND NOT digitalWrite()?
 *  The onboard buzzer is a PASSIVE piezo. It has no oscillator inside, so a steady
 *  HIGH just deflects the disc once and holds it there -- you get a faint click,
 *  then silence. It needs a square wave, which is what tone() generates.
 *  (An ACTIVE buzzer is the opposite: it has its own oscillator, so digitalWrite()
 *  HIGH is all it needs and tone() would be wrong.)
 *
 *  No libraries to install. Upload, then open Serial Monitor at 115200 baud.
 *
 *  Everything here is non-blocking: the note sequencer runs off millis(), so you can
 *  paste it straight into the attendance project without stalling the card reader.
 **************************************************************************************/

#define PIN_BUZZER 26        // onboard passive piezo on the Maker ESP32

/* ===================================================================================
 *  A tiny non-blocking note sequencer
 *  A pattern is just an array of {frequency, duration}. freq 0 = silence (a rest).
 * =================================================================================== */
struct Note {
  uint16_t freq;   // Hz, or 0 for a rest
  uint16_t ms;     // how long to hold it
};

const Note *seqData    = nullptr;
uint8_t     seqLen     = 0;
uint8_t     seqIdx     = 0;
uint32_t    tNextNote  = 0;
bool        seqRunning = false;

// Start a pattern. Returns immediately -- the sound happens in serviceTone().
void playPattern(const Note *notes, uint8_t len) {
  seqData    = notes;
  seqLen     = len;
  seqIdx     = 0;
  tNextNote  = millis();
  seqRunning = true;
}

// Call this every loop(). It advances the pattern by at most one note.
void serviceTone() {
  if (!seqRunning) return;
  if ((int32_t)(millis() - tNextNote) < 0) return;   // current note still playing

  if (seqIdx >= seqLen) {                            // pattern finished
    noTone(PIN_BUZZER);
    seqRunning = false;
    Serial.println("   done");
    return;
  }

  const Note &n = seqData[seqIdx];
  if (n.freq == 0) noTone(PIN_BUZZER);
  else             tone(PIN_BUZZER, n.freq);

  tNextNote = millis() + n.ms;
  seqIdx++;
}

/* ===================================================================================
 *  The patterns the attendance system actually uses
 * =================================================================================== */

// 1 - card detected: one short chirp, fires the instant the UID is read
const Note P_TAP[] = { {2500, 60} };

// 2 - accepted: two quick rising notes = "welcome"
const Note P_OK[] = { {2000, 70}, {0, 40}, {2800, 90} };

// 3 - duplicate tap: one flat mid note = "already recorded"
const Note P_DUP[] = { {1200, 300} };

// 4 - unknown card: three low notes = "no"
const Note P_ERR[] = { {600, 180}, {0, 90}, {600, 180}, {0, 90}, {600, 260} };

// 5 - boot chirp
const Note P_BOOT[] = { {1800, 90}, {0, 50}, {2600, 130} };

// 6 - frequency sweep, to find where YOUR piezo is loudest
const Note P_SWEEP[] = {
  {1000, 200}, {1500, 200}, {2000, 200}, {2500, 200},
  {3000, 200}, {3500, 200}, {4000, 200}, {4500, 200}
};

// 7 - C major scale, just to prove pitch control works
const Note P_SCALE[] = {
  {262, 180}, {294, 180}, {330, 180}, {349, 180},
  {392, 180}, {440, 180}, {494, 180}, {523, 320}
};

#define LEN(a) (sizeof(a) / sizeof(a[0]))

/* ===================================================================================
 *  Serial menu
 * =================================================================================== */
void printMenu() {
  Serial.println();
  Serial.println("=== Maker ESP32 onboard buzzer test (GPIO26) ===");
  Serial.println("  1 = card tapped      (short chirp)");
  Serial.println("  2 = accepted         (two rising notes)");
  Serial.println("  3 = duplicate tap    (one flat note)");
  Serial.println("  4 = unknown card     (three low notes)");
  Serial.println("  5 = boot chirp");
  Serial.println("  6 = frequency sweep  (1k -> 4.5k, find the loudest)");
  Serial.println("  7 = C major scale");
  Serial.println("  0 = stop / silence");
  Serial.println("  m = show this menu");
  Serial.println("Heard nothing at all? Check the MUTE SWITCH on the board.");
  Serial.print("> ");
}

void handleKey(char c) {
  switch (c) {
    case '1': Serial.println("tap");        playPattern(P_TAP,   LEN(P_TAP));   break;
    case '2': Serial.println("accepted");   playPattern(P_OK,    LEN(P_OK));    break;
    case '3': Serial.println("duplicate");  playPattern(P_DUP,   LEN(P_DUP));   break;
    case '4': Serial.println("unknown");    playPattern(P_ERR,   LEN(P_ERR));   break;
    case '5': Serial.println("boot");       playPattern(P_BOOT,  LEN(P_BOOT));  break;
    case '6': Serial.println("sweep");      playPattern(P_SWEEP, LEN(P_SWEEP)); break;
    case '7': Serial.println("scale");      playPattern(P_SCALE, LEN(P_SCALE)); break;
    case '0': Serial.println("silence");    noTone(PIN_BUZZER); seqRunning = false; break;
    case 'm': case 'M': printMenu(); return;
    default:  return;                       // ignore newlines and stray characters
  }
  Serial.print("> ");
}

/* ===================================================================================
 *  SETUP / LOOP
 * =================================================================================== */
void setup() {
  Serial.begin(115200);
  delay(300);                          // one-off, so the first print is not cut off

  pinMode(PIN_BUZZER, OUTPUT);         // harmless alongside tone(), keeps intent clear

  printMenu();
  playPattern(P_BOOT, LEN(P_BOOT));    // you should hear this the moment it boots
}

void loop() {
  // --- read the Serial menu without ever waiting ---
  while (Serial.available()) {
    handleKey((char)Serial.read());
  }

  // --- advance whatever pattern is playing ---
  serviceTone();

  // Nothing here blocks, so you can add your own code below and it will still run
  // smoothly while the buzzer is sounding.
}
