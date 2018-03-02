#include <SPI.h>
#include <Wire.h>
#include <TimeLib.h>
#include "rtc.h"
#include "config.h"

#define DISPLAY_DELAY_MICROS 5000

#define MASK_UPPER_DOTS 1
#define MASK_LOWER_DOTS 2

#define PIN_LE 10 /* Latch Enabled data accepted while HI level */
#define PIN_HIZ 8 /* Z state in registers outputs (while LOW level) */
#define PIN_DHV 5 /* off/on MAX1771 Driver  Hight Voltage(DHV) 110-220V */
#define PIN_BUZZER 2

#define PIN_LED_RED 9
#define PIN_LED_GREEN 6
#define PIN_LED_BLUE 3

#define PIN_BUTTON_SET A0
#define PIN_BUTTON_UP A2
#define PIN_BUTTON_DOWN A1

#define ONE_SECOND_IN_MS (1000UL)
#define ONE_MINUTE_IN_MS (ONE_SECOND_IN_MS * 60UL)
#define ONE_HOUR_IN_MS (ONE_MINUTE_IN_MS * 60UL)

#define ALL_TUBES ((1 << 10) - 1)
#define NO_TUBES 0
#define getNumber(idx) (1 << ((idx) % 10))

#ifdef EFFECT_SLOT_MACHINE
#define EFFECT_ENABLED
#endif

#ifdef EFFECT_ENABLED
byte dataIsTransitioning[6] = {0, 0, 0, 0, 0, 0};
uint16_t dataToDisplayOld[6] = {0, 0, 0, 0, 0, 0};
#endif
uint16_t dataToDisplay[6] = {0, 0, 0, 0, 0, 0}; // This will be displayed on tubes
byte dotMask;

String inputString;
unsigned long holdDisplayUntil;
bool colorSet;

unsigned long holdColorStartTime, holdColorEaseInTarget, holdColorSteadyTarget, holdColorEaseOutTarget;
byte setR, setG, setB;

unsigned long antiPoisonEnd;

bool stopwatchEnabled = false;
bool stopwatchRunning = false;
unsigned long prevMillis;
unsigned long stopwatchTime, countdownTo;

unsigned long RTCLastSyncTime;

void setup() {
  // Pin setup
  pinMode(PIN_DHV, OUTPUT);
  digitalWrite(PIN_DHV, LOW); // Turn off HV ASAP during setup

  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  analogWrite(PIN_LED_RED, 0);
  analogWrite(PIN_LED_GREEN, 0);
  analogWrite(PIN_LED_BLUE, 0);

  pinMode(PIN_BUZZER, OUTPUT);

  pinMode(PIN_LE, OUTPUT);
  pinMode(PIN_HIZ, OUTPUT);
  digitalWrite(PIN_HIZ, LOW);

  pinMode(PIN_BUTTON_SET, INPUT_PULLUP);
  pinMode(PIN_BUTTON_UP, INPUT_PULLUP);
  pinMode(PIN_BUTTON_DOWN, INPUT_PULLUP);

  // Lib setup
  Wire.begin();
  Serial.begin(115200);
  SPI.begin();

  // Begin initialization routines
  inputString.reserve(32);

  rtcTest();
  rtcSync();

  // Turn on HV
  digitalWrite(PIN_DHV, HIGH);

  Serial.println(F("< Ready"));
}

void serialEvent() {
  while (Serial.available()) {
    char inChar = (char)Serial.read();
    if (inChar != '\n') {
      inputString += inChar;
      if (inputString.length() >= 30) {
        inputString = "";
        Serial.println(F("< Serial line too long. Buffer reset."));
      }
      continue;
    }
    Serial.print(F("> "));
    Serial.println(inputString);

    byte tmpData;

    switch (inputString[0]) {
      // T HH II SS DD MM YY W
      // H = Hours, I = Minutes, S = Seconds, D = Day of month, M = month, Y = year, W = Day of week (ALL Dec)
      // Sets the time on the clock
      // T1756300103180
      case 'T':
        if (inputString.length() < 16) {
          Serial.println(F("T BAD (Invalid length; expected 16)"));
          break;
        }
        rtcSetTime(
          inputString.substring(1, 3).toInt(),   // H
          inputString.substring(3, 5).toInt(),   // I
          inputString.substring(5, 7).toInt(),   // S
          inputString.substring(6, 9).toInt(),   // D
          inputString.substring(9, 11).toInt(),  // M
          inputString.substring(11, 13).toInt(), // Y
          inputString.substring(13, 14).toInt()  // W
        );
        Serial.println(F("T OK"));
        break;
      // X
      // Performs a display reset of all modes
      case 'X':
        holdDisplayUntil = 0;
        noColor();
        stopwatchEnabled = false;
        stopwatchRunning = false;
        stopwatchTime = 0;
        countdownTo = 0;
        Serial.println(F("X OK"));
        break;
      // P CC
      // C = Count (Dec)
      // Performs an anti poisoning routine <C> times
      // P01
      case 'P':
        if (inputString.length() < 2) {
          Serial.println(F("P BAD (Invalid length; expected 2)"));
          break;
        }
        displayAntiPoison(inputString.substring(1, 3).toInt());
        Serial.println(F("P OK"));
        break;
      // G [MMMMMMMM IIII OOOO RR GG BB]
      // M = milliseconds (Dec), I = Ease-In (Dec), O = Ease-Out (Dec), R = Red (Hex), G = Green (Hex), B = Blue (Hex)
      // Shows a "flash"/"alert" color on the clock
      // If sent without any parameters, resets current flash color
      // G000010000500050000FF00
      case 'G':
        if (inputString.length() < 23) {
          if (inputString.length() < 3) { // Allow for \r\n
            noColor();
            Serial.println(F("G OK"));
            break;
          }
          Serial.println(F("G BAD (Invalid length; expected 23 or 1)"));
          break;
        }
        holdColorStartTime = millis();
        holdColorEaseInTarget = holdColorStartTime + (unsigned long)inputString.substring(9, 13).toInt();
        holdColorSteadyTarget = holdColorEaseInTarget + (unsigned long)inputString.substring(1, 9).toInt();
        holdColorEaseOutTarget = holdColorSteadyTarget + (unsigned long)inputString.substring(13, 17).toInt();

        setR = hexInputToByte(17);
        setG = hexInputToByte(19);
        setB = hexInputToByte(21);
        colorSet = true;
        Serial.println(F("G OK"));
        break;
      // F [MMMMMMMM D NNNNNN]
      // M = milliseconds (Dec), D = dots (Bitmask Dec) to show the message, N = Nixie message (Dec)
      // Shows a "flash"/"alert" message on the clock (will show this message instead of the time for <M> milliseconds. Does not use/reset hold when 0). Dots are bit 1 for lower and bit 2 for upper. Turned off when HIGH
      // If sent without any parameters, resets current flash message and goes back to clock mode
      // F0000100021337NA
      case 'F':
        if (inputString.length() < 16) {
          if (inputString.length() < 3) { // Allow for \r\n
            holdDisplayUntil = 0;
            Serial.println(F("F OK"));
            break;
          }
          Serial.println(F("F BAD (Invalid length; expected 16 or 1)"));
          break;
        }

        holdDisplayUntil = millis() + (unsigned long)inputString.substring(1, 9).toInt();
        tmpData = inputString[9] - '0';
        setDots((tmpData & 2) == 2, (tmpData & 1) == 1);
        for (int i = 0; i < 6; i++) {
          tmpData = inputString[i + 10];
          if (tmpData == 'N') {
            dataToDisplay[i] = NO_TUBES;
          } else if (tmpData == 'A') {
            dataToDisplay[i] = ALL_TUBES;
          } else {
            dataToDisplay[i] = getNumber(tmpData - '0');
          }
        }

        antiPoisonEnd = 0;

        Serial.println(F("F OK"));
        break;
      // C MMMMMMMM
      // M = Time in ms (Dec)
      // Starts a countdown for <M> ms. Stops countdown if <M> = 0
      // C00010000
      case 'C':
        if (inputString.length() < 9) {
          countdownTo = 0;
        } else {
          stopwatchEnabled = false;
          stopwatchRunning = false;
          stopwatchTime = 0;
          countdownTo = millis() + inputString.substring(1, 9).toInt();
        }
        Serial.println(F("C OK"));
        break;
      // W C
      // C = subcommand
      // Controls the stopwatch. R for reset/disable, P for pause, U for un-pause, S for start/restart
      // WS
      case 'W':
        if (inputString.length() < 2) {
          Serial.println(F("W BAD (Invalid length; expected 2)"));
          break;
        }
        tmpData = true;
        switch (inputString[1]) {
          case 'R':
            stopwatchEnabled = false;
            stopwatchRunning = false;
            stopwatchTime = 0;
            break;
          case 'P':
            stopwatchRunning = false;
            break;
          case 'U':
            stopwatchRunning = true;
            break;
          case 'S':
            countdownTo = 0;
            stopwatchEnabled = true;
            stopwatchRunning = true;
            stopwatchTime = 0;
            break;
          default:
            tmpData = false;
            Serial.print(F("W BAD (Invalid C)"));
            break;
        }
        if (tmpData) {
          Serial.println(F("W OK"));
        }
        break;
    }

    inputString = "";
  }
}

byte hexInputToByte(int offset) {
  byte msn = inputString[offset];
  msn = (msn <= '9') ? msn - '0' : msn - '7';
  byte lsn = inputString[offset + 1];
  lsn = (lsn <= '9') ? lsn - '0' : lsn - '7';
  return (msn << 4) + lsn;
}

void setDots(bool upper, bool lower) {
  dotMask = (upper ? 0 : MASK_UPPER_DOTS) | (lower ? 0 : MASK_LOWER_DOTS);
}

void noColor() {
  colorSet = false;
  analogWrite(PIN_LED_RED, 0);
  analogWrite(PIN_LED_GREEN, 0);
  analogWrite(PIN_LED_BLUE, 0);
}

void displayAntiPoison(int count) {
  antiPoisonEnd = millis() + (unsigned long)(ANTI_POISON_DELAY * 10UL * count);
}

void loop() {
  bool displayDirty = false;
  unsigned long curMillis = millis();
  unsigned long milliDelta = curMillis - prevMillis;
  if (curMillis < prevMillis) {
    milliDelta = 0;
  }
  prevMillis = curMillis;

  if (curMillis - RTCLastSyncTime >= 10000 || curMillis < RTCLastSyncTime) {
    rtcSync();
    RTCLastSyncTime = curMillis;
  }

  // Handle color logic
  if (colorSet) {
    float factor = 1.0;
    if (curMillis < holdColorEaseInTarget) {
      factor = 1.0 - ((float)(holdColorEaseInTarget - curMillis) / (float)(holdColorEaseInTarget - holdColorStartTime));
    } else if (curMillis > holdColorEaseOutTarget) {
      colorSet = false;
      factor = 0.0;
    } else if (curMillis > holdColorSteadyTarget) {
      factor = (float)(holdColorEaseOutTarget - curMillis) / (float)(holdColorEaseOutTarget - holdColorSteadyTarget);
    }
    analogWrite(PIN_LED_RED, setR * factor);
    analogWrite(PIN_LED_GREEN, setG * factor);
    analogWrite(PIN_LED_BLUE, setB * factor);
  }

  // Handle other stuff
  if (stopwatchRunning) {
    stopwatchTime += milliDelta;
  }

  // Handle "what to display" logic
  if (antiPoisonEnd > curMillis) {
    uint16_t sym = getNumber((antiPoisonEnd - curMillis) / ANTI_POISON_DELAY);
    for (int i = 0; i < 6; i++) {
      dataToDisplay[i] = sym;
    }
    displayDirty = true;
  } else if (holdDisplayUntil <= curMillis) {
    holdDisplayUntil = curMillis + 10;
    if (countdownTo > 0) {
      if (countdownTo <= curMillis) {
        displayDirty = showShortTime(0, true);
      } else {
        displayDirty = showShortTime(countdownTo - curMillis, true);
      }
    } else if (stopwatchEnabled) {
      displayDirty = showShortTime(stopwatchTime, true);
    } else {
      time_t _n = now();
      byte h = hour(_n);
      byte s = second(_n);

      if (s % 2) {
        setDots(true, true);
      } else {
        setDots(false, false);
      }

#ifdef CLOCK_TRIM_HOURS
      insert1(0, h / 10, true);
      insert1(1, h, false);
#else
      insert2(0, h, false);
#endif
      insert2(2, minute(_n), false);
      insert2(4, s, false);

      if (h < 4 && s % 10 == 0) {
        displayAntiPoison(1);
      }

      displayDirty = true;
    }
  }

#ifdef EFFECT_ENABLED
  if (displayDirty) {
    for (int i = 0; i < 6; i++) {
      if (dataToDisplayOld[i] != dataToDisplay[i]) {
        dataToDisplayOld[i] = dataToDisplay[i];
        dataIsTransitioning[i] = EFFECT_SPEED;
      }
    }
  }
#endif

  renderNixies(milliDelta);
}

bool showShortTime(unsigned long timeMs, bool trimLZ) {
  if (timeMs >= ONE_HOUR_IN_MS) { // Show H/M/S
    setDots(true, false);
    trimLZ = insert2(0, (timeMs / ONE_HOUR_IN_MS) % 100, trimLZ);
    trimLZ = insert2(2, (timeMs / ONE_MINUTE_IN_MS) % 60, trimLZ);
    insert2(4, (timeMs / ONE_SECOND_IN_MS) % 60, trimLZ);
    return true;
  } else { // Show M/S/MS
    setDots(false, true);
    trimLZ = insert2(0, (timeMs / ONE_MINUTE_IN_MS) % 60, trimLZ);
    trimLZ = insert2(2, (timeMs / ONE_SECOND_IN_MS) % 60, trimLZ);
    insert2(4, (timeMs / 10UL) % 100, trimLZ);
    return false; // Don't allow transition effects on rapid timer
  }
}

bool insert1(int offset, int data, bool trimLeadingZero) {
  data %= 10;
  if (data == 0 && trimLeadingZero) {
    dataToDisplay[offset] = 0;
    return true;
  } else {
    dataToDisplay[offset] = getNumber(data);
    return false;
  }
}

bool insert2(int offset, int data, bool trimLeadingZero) {
  trimLeadingZero = insert1(offset, data / 10, trimLeadingZero);
  return insert1(offset + 1, data, trimLeadingZero);
}

void displaySelfTest() {
  Serial.println(F("< Start LED Test"));

  setDots(true, true);

  analogWrite(PIN_LED_RED, 255);
  delay(1000);
  analogWrite(PIN_LED_RED, 0);
  analogWrite(PIN_LED_GREEN, 255);
  delay(1000);
  analogWrite(PIN_LED_GREEN, 0);
  analogWrite(PIN_LED_BLUE, 255);
  delay(1000);
  analogWrite(PIN_LED_BLUE, 0);

  Serial.println(F("< Stop LED Test"));

  displayAntiPoison(2);
}

void renderNixies(unsigned long milliDelta) {
  static byte anodeGroup = 0;
  static unsigned long lastTimeInterval1Started;

  unsigned long curMicros = micros();
  if (curMicros >= lastTimeInterval1Started) {
    unsigned long timeSinceLastRender = curMicros - lastTimeInterval1Started;
    if (timeSinceLastRender < DISPLAY_DELAY_MICROS) {
#ifdef RENDER_USE_DELAY
      delayMicroseconds(DISPLAY_DELAY_MICROS - timeSinceLastRender);
#else // RENDER_USE_DELAY
      return;
#endif // RENDER_USE_DELAY
    }
  } else if (curMicros < DISPLAY_DELAY_MICROS) {
#ifdef RENDER_USE_DELAY
      delayMicroseconds(DISPLAY_DELAY_MICROS - curMicros);
#else // RENDER_USE_DELAY
      return;
#endif // RENDER_USE_DELAY
  }
  lastTimeInterval1Started = curMicros;

  byte curTubeL = anodeGroup << 1;
  byte curTubeR = curTubeL + 1;

  uint16_t tubeL = dataToDisplay[curTubeL];
  uint16_t tubeR = dataToDisplay[curTubeR];

#ifdef EFFECT_ENABLED
  byte tubeTrans = dataIsTransitioning[curTubeL];
  if (tubeTrans > 0) {
#ifdef EFFECT_SLOT_MACHINE
    tubeL = getNumber(tubeTrans / (EFFECT_SPEED / 10));
#endif
    if (tubeTrans > milliDelta) {
      dataIsTransitioning[curTubeL] -= milliDelta;
    } else {
      dataIsTransitioning[curTubeL] = 0;
    }
  }

  tubeTrans = dataIsTransitioning[curTubeR];
  if (tubeTrans > 0) {
#ifdef EFFECT_SLOT_MACHINE
    tubeR = getNumber(tubeTrans / (EFFECT_SPEED / 10));
#endif
    if (tubeTrans > milliDelta) {
      dataIsTransitioning[curTubeR] -= milliDelta;
    } else {
      dataIsTransitioning[curTubeR] = 0;
    }
  }
#endif

  digitalWrite(PIN_LE, LOW); // allow data input (Transparent mode)
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE2));
  SPI.transfer(dotMask);                            // [   ][   ][   ][   ][   ][   ][L1 ][L0 ] - L0     L1 - dots
  SPI.transfer(tubeR >> 6 | 1 << (anodeGroup + 4)); // [   ][A2 ][A1 ][A0 ][RC9][RC8][RC7][RC6] - A0  -  A2 - anodes
  SPI.transfer(tubeR << 2 | tubeL >> 8);            // [RC5][RC4][RC3][RC2][RC1][RC0][LC9][LC8] - RC9 - RC0 - Right tubes cathodes
  SPI.transfer(tubeL);                              // [LC7][LC6][LC5][LC4][LC3][LC2][LC1][LC0] - LC9 - LC0 - Left tubes cathodes
  SPI.endTransaction();
  digitalWrite(PIN_LE, HIGH); // latching data

  if (++anodeGroup > 2) {
    anodeGroup = 0;
  }
}

