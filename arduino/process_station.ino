/*
  ==========================================================================
   MINI PROCESS STATION - Temperature control with local HMI and SCADA
   Board: Funduino / Arduino UNO R3
  ==========================================================================
   What it does:
   - Reads temperature and humidity (DHT11), light (LDR) and presence
     (obstacle sensor).
   - AUTO mode: if T > setpoint (SP) the relay turns on (fan/pump).
     It turns off when T drops below SP - 0.5 C (hysteresis).
   - MANUAL mode: you drive the relay yourself (IR remote or serial port).
   - ALARM: if T > SP + 3 C the buzzer sounds and the red LED turns on.
     The alarm stays latched until you reset it (IR remote or RESET)
     and the temperature has dropped. Green LED = everything OK.
   - IR remote: POWER = run/stop, MODE = AUTO/MANUAL, +/- = setpoint,
     left/right = pages, MUTE = silence the buzzer,
     EQ = alarm reset, 1/0 = relay ON/OFF (MANUAL only).
   - LCD buttons: SELECT = AUTO/MANUAL, UP/DOWN = setpoint +/- 0.5 C,
     LEFT/RIGHT = change page.
   - Every second it sends a JSON line over USB (115200 baud) for
     Node-RED / Ignition. Lines starting with '#' are diagnostic
     messages (not process data).

   Serial commands (terminated with Enter):
     SP=27.5          -> change the setpoint
     MODE=AUTO        -> automatic mode
     MODE=MANUAL      -> manual mode
     RELAY=1 / RELAY=0-> relay ON/OFF (MANUAL only)
     RESET            -> alarm reset (only if T has already dropped)
     MUTE             -> silence the buzzer
     TEST             -> test relay, buzzer, red LED and green LED (1 s each)
     RUN=1 / RUN=0    -> general run / stop
     The older MODO= and RELE= forms are still accepted.

   Libraries (IDE Library Manager):
     "DHT sensor library" (Adafruit), "Adafruit Unified Sensor",
     "IRremote" (version 4.x). LiquidCrystal already ships with the IDE.
  ==========================================================================
*/

// Decode NEC only (the protocol used by the kit remotes): saves memory
#define DECODE_NEC
#include <IRremote.hpp>
#include <LiquidCrystal.h>
#include <DHT.h>

// ------------------------- QUICK SETTINGS --------------------------------
// If the relay works backwards (it turns on when it should be off),
// change HIGH to LOW on the next line.
#define RELAY_ACTIVE    HIGH
// If the buzzer sounds backwards (it beeps when idle), change HIGH to LOW.
#define BUZZER_ACTIVE   HIGH
// If the light reading is inverted (more light = lower %), change 1 to 0.
#define LIGHT_INVERTED  1

// Initial setpoint and limits (degrees C)
#define SP_INITIAL      26.0
#define SP_MIN          10.0
#define SP_MAX          50.0
#define SP_STEP         0.5
#define HYSTERESIS      0.5
#define ALARM_MARGIN    3.0

// ------------------------- IR REMOTE CODES -------------------------------
// Typical values for the 21-key "Car MP3" remote (NEC protocol).
// If your remote is different: open the Serial Monitor (115200), press
// each key and you will see "# IR command=0x.."; copy that value here
// and upload again.
#define IR_POWER   0x45   // POWER: general run/stop
#define IR_MODE    0x46   // MODE: AUTO/MANUAL
#define IR_MUTE    0x47   // MUTE: silence the alarm buzzer
#define IR_LEFT    0x40   // left: previous page
#define IR_RIGHT   0x43   // right: next page
#define IR_EQ      0x07   // EQ: alarm reset (if T has already dropped)
#define IR_PLUS    0x09   // +: raise setpoint 0.5 C
#define IR_MINUS   0x15   // -: lower setpoint 0.5 C
#define IR_1       0x0C   // 1: relay ON (MANUAL)
#define IR_0       0x16   // 0: relay OFF (MANUAL)

// ------------------------- PINS ------------------------------------------
// LCD Keypad Shield (wired with jumpers): RS, E, D4, D5, D6, D7
#define PIN_LCD_RS   8
#define PIN_LCD_E    9
#define PIN_BUTTONS  A0
#define PIN_DHT      2
#define PIN_OBST     3    // KY-032: LOW = obstacle present
#define PIN_LDR      A1
#define PIN_IR       A2
#define PIN_LED_GREEN A3
#define PIN_RELAY    11
#define PIN_BUZZER   12
#define PIN_LED_RED  13

LiquidCrystal lcd(PIN_LCD_RS, PIN_LCD_E, 4, 5, 6, 7);
DHT dht(PIN_DHT, DHT11);

// ------------------------- PROCESS VARIABLES -----------------------------
float temperature = NAN;
float humidity = NAN;
bool  dhtOk = false;
int   lightPct = 0;
bool  obstacle = false;

float setpoint = SP_INITIAL;
bool  autoMode = true;
bool  running = true;            // general run (RUN)
bool  relay = false;             // relay state
bool  manualRelay = false;       // manual relay command
bool  alarm = false;             // latched alarm
bool  alarmAcknowledged = false; // buzzer silenced with MUTE

byte  page = 0;                  // 0, 1 or 2
#define NUM_PAGES 3

// Timers (all based on millis, no delay in the main loop)
unsigned long tDht = 0, tJson = 0, tLcd = 0, tBuzzer = 0, tSensors = 0;
bool buzzerPhase = false;

// Serial port
char serialBuf[33];
byte serialLen = 0;

// Buttons
enum Button { NONE, RIGHT, UP, DOWN, LEFT, SELECT };
Button stableButton = NONE, readButton = NONE;
unsigned long tButton = 0;

// ==========================================================================
void setup() {
  // Relay off BEFORE it is set as an output (avoids a click on startup)
  digitalWrite(PIN_RELAY, !RELAY_ACTIVE);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_BUZZER, !BUZZER_ACTIVE);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_OBST, INPUT_PULLUP);

  Serial.begin(115200);
  lcd.begin(16, 2);
  lcd.print(F("Process Station"));
  lcd.setCursor(0, 1);
  lcd.print(F("Starting...     "));

  dht.begin();
  IrReceiver.begin(PIN_IR, DISABLE_LED_FEEDBACK);

  Serial.println(F("# Process station ready. 115200 baud."));
  Serial.println(F("# Commands: SP=27.5 MODE=AUTO|MANUAL RELAY=1|0 RESET MUTE TEST RUN=1|0"));
  tDht = millis() - 1500;   // first DHT reading after 0.5 s
}

// ==========================================================================
void loop() {
  unsigned long now = millis();

  // --- DHT11 reading every 2 s ---
  if (now - tDht >= 2000) {
    tDht = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h)) {
      dhtOk = false;
      Serial.println(F("# DHT11 read error (check the cable on D2)"));
    } else if (dhtOk && (h < 20 || abs(h - humidity) > 15)) {
      // odd DHT11 reading (jumps down to 13-19 %): discarded
    } else {
      dhtOk = true;
      temperature = t;
      humidity = h;
    }
  }

  // --- Light and obstacle every 100 ms ---
  if (now - tSensors >= 100) {
    tSensors = now;
    int v = analogRead(PIN_LDR);
    lightPct = map(v, 0, 1023, 0, 100);
    if (LIGHT_INVERTED) lightPct = 100 - lightPct;
  }
  // Obstacle: checked every loop and held for 1.5 s so it is visible
  static unsigned long tObstacle = 0;
  if (digitalRead(PIN_OBST) == LOW) tObstacle = now;
  obstacle = (tObstacle != 0 && now - tObstacle < 1500);

  readButtons(now);
  readIR();
  readSerial();
  controlLogic();
  updateOutputs(now);

  // --- Display every 250 ms ---
  if (now - tLcd >= 250) {
    tLcd = now;
    drawLcd();
  }

  // --- JSON every 1 s ---
  if (now - tJson >= 1000) {
    tJson = now;
    sendJson();
  }
}

// ========================== CONTROL LOGIC =================================
void controlLogic() {
  // Alarm: latches if T > SP + 3
  bool alarmCondition = dhtOk && (temperature > setpoint + ALARM_MARGIN);
  if (alarmCondition && !alarm) {
    alarm = true;
    alarmAcknowledged = false;
    Serial.println(F("# ALARM: high temperature"));
  }

  if (!running) {
    relay = false;                      // general stop: relay always off
  } else if (autoMode) {
    if (!dhtOk) {
      relay = false;                    // no reliable reading -> fail-safe
    } else if (temperature > setpoint) {
      relay = true;
    } else if (temperature <= setpoint - HYSTERESIS) {
      relay = false;
    }                                   // in between: hold (hysteresis)
  } else {
    relay = manualRelay;
  }
}

void resetAlarm() {
  bool alarmCondition = dhtOk && (temperature > setpoint + ALARM_MARGIN);
  if (!alarm) {
    Serial.println(F("# RESET: no alarm active"));
  } else if (alarmCondition) {
    Serial.println(F("# RESET: not possible, T is still high (use MUTE to silence the buzzer)"));
  } else {
    alarm = false;
    alarmAcknowledged = false;
    Serial.println(F("# RESET: alarm cleared"));
  }
}

void updateOutputs(unsigned long now) {
  digitalWrite(PIN_RELAY, relay ? RELAY_ACTIVE : !RELAY_ACTIVE);
  digitalWrite(PIN_LED_RED, alarm ? HIGH : LOW);
  digitalWrite(PIN_LED_GREEN, alarm ? LOW : HIGH);

  // Intermittent buzzer (0.5 s on / 0.5 s off) while the alarm is not acknowledged
  if (alarm && !alarmAcknowledged) {
    if (now - tBuzzer >= 500) {
      tBuzzer = now;
      buzzerPhase = !buzzerPhase;
    }
  } else {
    buzzerPhase = false;
  }
  soundBuzzer(buzzerPhase);
}

// Software square wave (~2 kHz): works with both active and passive buzzers
// (tone() is not used because it shares a timer with the IR receiver)
void soundBuzzer(bool on) {
  static unsigned long tUs = 0;
  static bool level = false;
  if (!on) { digitalWrite(PIN_BUZZER, !BUZZER_ACTIVE); level = false; return; }
  unsigned long us = micros();
  if (us - tUs >= 250) { tUs = us; level = !level; digitalWrite(PIN_BUZZER, level); }
}

// ========================== LCD BUTTONS ===================================
Button buttonFromAnalog(int v) {
  // Wide thresholds: work across the different shield revisions
  // Calibrated on this shield: RIGHT 0, UP 100-166, DOWN 256-371, LEFT 408, SELECT 637
  if (v < 50)  return RIGHT;
  if (v < 210) return UP;
  if (v < 390) return DOWN;
  if (v < 520) return LEFT;
  if (v < 850) return SELECT;
  return NONE;                   // ~1023
}

void readButtons(unsigned long now) {
  int vA0 = analogRead(PIN_BUTTONS);
  Button b = buttonFromAnalog(vA0);
  if (b != readButton && b != NONE) {  // diagnostic: A0 value on press
    const char* names[] = {"NONE","RIGHT","UP","DOWN","LEFT","SELECT"};
    Serial.print(F("# Button A0="));
    Serial.print(vA0);
    Serial.print(F(" -> "));
    Serial.println(names[(int)b]);
  }
  if (b != readButton) {           // 80 ms debounce
    readButton = b;
    tButton = now;
    return;
  }
  if (now - tButton < 80 || b == stableButton) return;
  stableButton = b;
  if (b == NONE) return;           // act only on press

  switch (b) {
    case SELECT:
      toggleMode(!autoMode);
      break;
    case UP:
      changeSetpoint(setpoint + SP_STEP);
      page = 2;
      break;
    case DOWN:
      changeSetpoint(setpoint - SP_STEP);
      page = 2;
      break;
    case RIGHT:
      page = (page + 1) % NUM_PAGES;
      break;
    case LEFT:
      page = (page + NUM_PAGES - 1) % NUM_PAGES;
      break;
    default: break;
  }
}

void toggleMode(bool toAuto) {
  autoMode = toAuto;
  manualRelay = relay;             // when switching to MANUAL, keep the current state
  Serial.print(F("# Mode: "));
  Serial.println(autoMode ? F("AUTO") : F("MANUAL"));
}

void changeSetpoint(float sp) {
  if (sp < SP_MIN) sp = SP_MIN;
  if (sp > SP_MAX) sp = SP_MAX;
  setpoint = sp;
  Serial.print(F("# Setpoint: "));
  Serial.println(setpoint, 1);
}

void muteAlarm() {
  if (!alarm) { Serial.println(F("# MUTE: no alarm active")); return; }
  alarmAcknowledged = true;
  Serial.println(F("# MUTE: buzzer silenced (alarm stays latched until EQ/RESET)"));
}

void testOutputs() {
  Serial.println(F("# TEST relay"));      digitalWrite(PIN_RELAY, RELAY_ACTIVE);    delay(1000); digitalWrite(PIN_RELAY, !RELAY_ACTIVE);
  Serial.println(F("# TEST buzzer"));
  for (unsigned long t0 = millis(); millis() - t0 < 1000;) soundBuzzer(true);
  soundBuzzer(false);
  digitalWrite(PIN_LED_GREEN, LOW);
  Serial.println(F("# TEST red LED (D13)"));   digitalWrite(PIN_LED_RED, HIGH);   delay(1000); digitalWrite(PIN_LED_RED, LOW);
  Serial.println(F("# TEST green LED (A3)"));  digitalWrite(PIN_LED_GREEN, HIGH); delay(1000); digitalWrite(PIN_LED_GREEN, LOW);
  Serial.println(F("# TEST done"));
}

void setRun(bool m) {
  running = m;
  if (!running) manualRelay = false;
  Serial.println(running ? F("# RUN") : F("# STOP"));
}

// ========================== IR REMOTE =====================================
void readIR() {
  if (!IrReceiver.decode()) return;
  if (IrReceiver.decodedIRData.protocol == UNKNOWN) { IrReceiver.resume(); return; }  // noise -> drop the 0x00
  bool isRepeat = IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT;
  uint8_t cmd = IrReceiver.decodedIRData.command;
  IrReceiver.resume();
  if (isRepeat) return;            // ignore a held key

  Serial.print(F("# IR command=0x"));
  if (cmd < 0x10) Serial.print('0');
  Serial.println(cmd, HEX);

  switch (cmd) {
    case IR_POWER: setRun(!running); break;
    case IR_MODE:  toggleMode(!autoMode); break;
    case IR_MUTE:  muteAlarm(); break;
    case IR_EQ:    resetAlarm(); break;
    case IR_PLUS:  changeSetpoint(setpoint + SP_STEP); page = 2; break;
    case IR_MINUS: changeSetpoint(setpoint - SP_STEP); page = 2; break;
    case IR_RIGHT: page = (page + 1) % NUM_PAGES; break;
    case IR_LEFT:  page = (page + NUM_PAGES - 1) % NUM_PAGES; break;
    case IR_1:
      if (!autoMode) { manualRelay = true;  Serial.println(F("# Relay ON (manual)")); }
      else Serial.println(F("# Relay: switch to MANUAL first (MODE)"));
      break;
    case IR_0:
      if (!autoMode) { manualRelay = false; Serial.println(F("# Relay OFF (manual)")); }
      else Serial.println(F("# Relay: switch to MANUAL first (MODE)"));
      break;
    default: break;
  }
}

// ========================== SERIAL PORT ===================================
void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLen > 0) {
        serialBuf[serialLen] = '\0';
        processCommand(serialBuf);
        serialLen = 0;
      }
    } else if (serialLen < sizeof(serialBuf) - 1) {
      if (c >= 'a' && c <= 'z') c -= 32;   // to uppercase
      if (c != ' ') serialBuf[serialLen++] = c;
    }
  }
}

void processCommand(char *cmd) {
  if (strncmp(cmd, "SP=", 3) == 0) {
    changeSetpoint(atof(cmd + 3));
  } else if (strcmp(cmd, "MODE=AUTO") == 0 || strcmp(cmd, "MODO=AUTO") == 0) {
    toggleMode(true);
  } else if (strcmp(cmd, "MODE=MANUAL") == 0 || strcmp(cmd, "MODO=MANUAL") == 0) {
    toggleMode(false);
  } else if (strcmp(cmd, "RELAY=1") == 0 || strcmp(cmd, "RELAY=0") == 0 ||
             strcmp(cmd, "RELE=1") == 0 || strcmp(cmd, "RELE=0") == 0) {
    if (autoMode) {
      Serial.println(F("# ERR: RELAY is only available in MANUAL mode"));
    } else {
      manualRelay = (cmd[strlen(cmd) - 1] == '1');
      Serial.println(manualRelay ? F("# Relay ON (manual)") : F("# Relay OFF (manual)"));
    }
  } else if (strcmp(cmd, "RESET") == 0) {
    resetAlarm();
  } else if (strcmp(cmd, "MUTE") == 0) {
    muteAlarm();
  } else if (strcmp(cmd, "TEST") == 0) {
    testOutputs();
  } else if (strcmp(cmd, "RUN=1") == 0) {
    setRun(true);
  } else if (strcmp(cmd, "RUN=0") == 0) {
    setRun(false);
  } else {
    Serial.print(F("# ERR: unknown command: "));
    Serial.println(cmd);
  }
}

// ========================== JSON ==========================================
void sendJson() {
  Serial.print(F("{\"t\":"));
  if (dhtOk) Serial.print(temperature, 1); else Serial.print(F("null"));
  Serial.print(F(",\"h\":"));
  if (dhtOk) Serial.print(humidity, 0); else Serial.print(F("null"));
  Serial.print(F(",\"light\":"));  Serial.print(lightPct);
  Serial.print(F(",\"obst\":"));   Serial.print(obstacle ? 1 : 0);
  Serial.print(F(",\"relay\":"));  Serial.print(relay ? 1 : 0);
  Serial.print(F(",\"mode\":\"")); Serial.print(autoMode ? F("AUTO") : F("MANUAL"));
  Serial.print(F("\",\"sp\":"));   Serial.print(setpoint, 1);
  Serial.print(F(",\"alarm\":"));  Serial.print(alarm ? 1 : 0);
  Serial.print(F(",\"run\":"));    Serial.print(running ? 1 : 0);
  Serial.println('}');
}

// ========================== LCD DISPLAY ===================================
// Writes a full 16-character line (pads with spaces, no flicker)
void lcdLine(byte row, const char *txt) {
  char l[17];
  snprintf(l, sizeof(l), "%-16s", txt);
  lcd.setCursor(0, row);
  lcd.print(l);
}

void drawLcd() {
  char l1[17], l2[17], num[8];

  if (page == 0) {
    // T:24.5C H:40%  !
    // AUTO  RUN  R:ON
    if (dhtOk) {
      dtostrf(temperature, 4, 1, num);
      snprintf(l1, sizeof(l1), "T:%sC H:%d%%", num, (int)humidity);
    } else {
      snprintf(l1, sizeof(l1), "T:--.-C ERR DHT");
    }
    snprintf(l2, sizeof(l2), "%-6s%-5sR:%s", autoMode ? "AUTO" : "MAN",
             running ? "RUN" : "STOP", relay ? "ON" : "OFF");
  } else if (page == 1) {
    // Light:63% Ob:Y
    // Alarm: OK
    snprintf(l1, sizeof(l1), "Light:%d%% Ob:%s", lightPct, obstacle ? "Y" : "N");
    snprintf(l2, sizeof(l2), "Alarm: %s",
             !alarm ? "OK" : (alarmAcknowledged ? "MUTE" : "ON"));
  } else {
    // Setpoint (SP)
    // 26.0C  +/- 0.5
    dtostrf(setpoint, 4, 1, num);
    snprintf(l1, sizeof(l1), "Setpoint (SP)");
    snprintf(l2, sizeof(l2), "%sC  +/- 0.5", num);
  }

  lcdLine(0, l1);
  lcdLine(1, l2);
  // Alarm indicator in the corner, whichever page is shown
  if (alarm) {
    lcd.setCursor(15, 0);
    lcd.print('!');
  }
}
