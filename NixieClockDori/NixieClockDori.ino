#include <SPI.h>
#include <TimeLib.h>
#include <SoftTimer.h>

#include "rtc.h"
#include "config.h"
#include "const.h"
#include "crcserial.h"
#include "Display.h"
#include "DisplayTask.h"

#include "DisplayTask_Clock.h"
#include "DisplayTask_Stopwatch.h"
#include "DisplayTask_Countdown.h"
#include "DisplayTask_Flash.h"

/****************/
/* PROGRAM CODE */
/****************/

/********************/
/* FUNCTION ALIASES */
/********************/

void renderNixies(Task *me);
void cycleDisplayUpdater(Task *me);
void serialReader(Task *me);

DisplayTask_Clock displayClock;
DisplayTask_Stopwatch displayStopwatch;
DisplayTask_Countdown displayCountdown;
DisplayTask_Flash displayFlash;

DisplayTask* displayUpdateTask;

Task T_renderNixies(5, renderNixies);
Task T_cycleDisplayUpdater(5000, cycleDisplayUpdater);
Task T_serialReader(0, serialReader);

/**************************/
/* ARDUINO EVENT HANDLERS */
/**************************/
void setup() {
	// Pin setup
	pinMode(PIN_HIGH_VOLTAGE_ENABLE, OUTPUT);
	digitalWrite(PIN_HIGH_VOLTAGE_ENABLE, LOW); // Turn off HV ASAP during setup

	pinMode(PIN_LED_RED, OUTPUT);
	pinMode(PIN_LED_GREEN, OUTPUT);
	pinMode(PIN_LED_BLUE, OUTPUT);
	analogWrite(PIN_LED_RED, 0);
	analogWrite(PIN_LED_GREEN, 0);
	analogWrite(PIN_LED_BLUE, 0);

	pinMode(PIN_BUZZER, OUTPUT);

	pinMode(PIN_DISPLAY_LATCH, OUTPUT);
	pinMode(PIN_HIZ, OUTPUT);
	digitalWrite(PIN_HIZ, LOW);

	pinMode(PIN_BUTTON_SET, INPUT_PULLUP);
	pinMode(PIN_BUTTON_UP, INPUT_PULLUP);
	pinMode(PIN_BUTTON_DOWN, INPUT_PULLUP);

	// Lib setup
	SPI.begin();

	// Begin initialization routines
	serialInit();
	rtcInit();

	digitalWrite(PIN_HIGH_VOLTAGE_ENABLE, HIGH);

	SoftTimer.add(&T_renderNixies);
	SoftTimer.add(&T_serialReader);
	SoftTimer.add(&T_cycleDisplayUpdater);

	displayClock.add();

	cycleDisplayUpdater(NULL);

	serialSend(F("< Ready"));
}

void serialReader(Task *me) {
	while (Serial.available()) {
		if (!serialReadNext()) {
			continue;
		}

		byte tmpData;

		switch (inputString[0]) {
			// T HH II SS DD MM YY W
			// H = Hours, I = Minutes, S = Seconds, D = Day of month, M = month, Y = year, W = Day of week (ALL Dec)
			// Sets the time on the clock
			// T1756300103180
		case 'T':
			if (inputString.length() < 14) {
				serialSend(F("T BAD (Invalid length; expected 16)"));
				break;
			}
			tmElements_t tm;
			tm.Hour = inputString.substring(1, 3).toInt();
			tm.Minute = inputString.substring(3, 5).toInt();
			tm.Second = inputString.substring(5, 7).toInt();
			tm.Day = inputString.substring(6, 9).toInt();
			tm.Month = inputString.substring(9, 11).toInt();
			tm.Year = inputString.substring(11, 13).toInt();
			tm.Wday = inputString.substring(13, 14).toInt();
			rtcSetTime(tm);
			serialSend(F("T OK"));
			break;
			// X
			// Performs a display reset of all modes
		case 'X':
			displayCountdown.to = 0;
			displayStopwatch.reset();
			if (!displayUpdateTask->canShow()) {
				cycleDisplayUpdater(NULL);
				T_cycleDisplayUpdater.lastCallTimeMicros = micros();
			}
			serialSend(F("X OK"));
			break;
			// P CC
			// C = Count (Dec)
			// Performs an anti poisoning routine <C> times
			// ^P01|-20043
		case 'P':
			if (inputString.length() < 2) {
				serialSend(F("P BAD (Invalid length; expected 2)"));
				break;
			}
			displayAntiPoison(inputString.substring(1, 3).toInt());
			serialSend(F("P OK"));
			break;
			// F [MMMMMMMM D NNNNNN [RR GG BB]]
			// M = milliseconds (Dec), D = dots (Bitmask Dec) to show the message, N = Nixie message (Dec), R = Red (Hex), G = Green (Hex), B = Blue (Hex)
			// Shows a "flash"/"alert" message on the clock (will show this message instead of the time for <M> milliseconds. Does not use/reset hold when 0). Dots are bit 1 for lower and bit 2 for upper. Turned off when HIGH
			// If sent without any parameters, resets current flash message and goes back to clock mode
			// ^F0000100021337NA|-15360
		case 'F':
			if (inputString.length() < 16) {
				if (inputString.length() < 3) { // Allow for \r\n
					cycleDisplayUpdater(NULL);
					serialSend(F("F OK"));
					break;
				}
				serialSend(F("F BAD (Invalid length; expected 16 or 1)"));
				break;
			}

			displayFlash.endTime = millis() + (unsigned long)inputString.substring(1, 9).toInt();

			tmpData = inputString[9] - '0';
			displayFlash.dotsMask = makeDotMask((tmpData & 2) == 2, (tmpData & 1) == 1);
			for (byte i = 0; i < 6; i++) {
				tmpData = inputString[i + 10];
				if (tmpData == 'N') {
					displayFlash.symbols[i] = NO_TUBES;
				}
				else if (tmpData == 'A') {
					displayFlash.symbols[i] = ALL_TUBES;
				}
				else {
					displayFlash.symbols[i] = getNumber(tmpData - '0');
				}
			}

			setColorFromInput(&displayFlash, 16);
			showIfPossibleOtherwiseRotateIfCurrent(&displayFlash);

			serialSend(F("F OK"));
			break;
			// C [MMMMMMMM [RR GG BB]]
			// M = Time in ms (Dec), R = Red (Hex), G = Green (Hex), B = Blue (Hex)
			// Starts a countdown for <M> ms. Stops countdown if <M> = 0
			// ^C00010000|9735
			// ^C|-26281
		case 'C':
			if (inputString.length() < 9) {
				displayCountdown.to = 0;
			}
			else {
				displayCountdown.to = millis() + inputString.substring(1, 9).toInt();
			}
			setColorFromInput(&displayCountdown, 9);
			showIfPossibleOtherwiseRotateIfCurrent(&displayCountdown);
			serialSend(F("C OK"));
			break;
			// W C [RR GG BB]
			// C = subcommand, R = Red (Hex), G = Green (Hex), B = Blue (Hex)
			// Controls the stopwatch. R for reset/disable, P for pause, U for un-pause, S for start/restart
			// ^WS|-8015
			// ^WR|-3952
		case 'W':
			if (inputString.length() < 2) {
				serialSend(F("W BAD (Invalid length; expected 2)"));
				break;
			}
			setColorFromInput(&displayStopwatch, 2);
			tmpData = true;
			switch (inputString[1]) {
			case 'R':
				displayStopwatch.reset();
				break;
			case 'P':
				displayStopwatch.pause();
				break;
			case 'U':
				displayStopwatch.resume();
				break;
			case 'S':
				displayStopwatch.start();
				break;
			default:
				tmpData = false;
				Serial.print(F("W BAD (Invalid C)"));
				break;
			}
			if (tmpData) {
				showIfPossibleOtherwiseRotateIfCurrent(&displayStopwatch);
				serialSend(F("W OK"));
			}
			break;
		}
	}
}

void setColorFromInput(DisplayTask *displayTask, const byte offset) {
	if (inputString.length() < offset + 6) {
		return;
	}
	displayTask->red = hexInputToByte(offset);
	displayTask->green = hexInputToByte(offset + 2);
	displayTask->blue = hexInputToByte(offset + 4);
}

void showIfPossibleOtherwiseRotateIfCurrent(DisplayTask *displayTask) {
	if (displayTask->canShow()) {
		displayTask->add();
		displayUpdateTask = displayTask;
	}
	else if (displayTask == displayUpdateTask) {
		cycleDisplayUpdater(NULL);
	}
	else {
		return;
	}
	T_cycleDisplayUpdater.lastCallTimeMicros = micros();
}

void cycleDisplayUpdater(Task *me) {
	displayUpdateTask = DisplayTask::findNextValid(displayUpdateTask);
}

void displayTriggerEffects() {
#ifdef EFFECT_ENABLED
	bool hasEffects = false;
	for (byte i = 0; i < 6; i++) {
		if (dataToDisplayOld[i] != dataToDisplay[i]) {
			dataToDisplayOld[i] = dataToDisplay[i];
			dataIsTransitioning[i] = EFFECT_SPEED;
			hasEffects = true;
		}
	}
#endif
}

void displayEffectsUpdate(const unsigned long microDelta) {
#ifdef EFFECT_ENABLED
	const unsigned long milliDelta = microDelta / 1000UL;
	bool hadEffects = false;
	for (byte i = 0; i < 6; i++) {
		if (dataIsTransitioning[i] > milliDelta) {
			dataIsTransitioning[i] -= milliDelta;
			hadEffects = true;
		}
		else {
			dataIsTransitioning[i] = 0;
		}
	}
#endif
}

/*********************/
/* UTILITY FUNCTIONS */
/*********************/
#define hexCharToNum(c) ((c <= '9') ? c - '0' : c - '7')
byte hexInputToByte(const byte offset) {
	const byte msn = inputString[offset];
	const byte lsn = inputString[offset + 1];
	return (hexCharToNum(msn) << 4) + hexCharToNum(lsn);
}

void displaySelfTest() {
	serialSend(F("< Start LED Test"));

	setDotsConst(true, true);

	analogWrite(PIN_LED_RED, 255);
	delay(1000);
	analogWrite(PIN_LED_RED, 0);
	analogWrite(PIN_LED_GREEN, 255);
	delay(1000);
	analogWrite(PIN_LED_GREEN, 0);
	analogWrite(PIN_LED_BLUE, 255);
	delay(1000);
	analogWrite(PIN_LED_BLUE, 0);

	serialSend(F("< Stop LED Test"));

	displayAntiPoison(2);
}

void renderNixies(Task *me) {
	static byte anodeGroup = 0;

	const unsigned long curMillis = millis();
	uint16_t tubeL = INVALID_TUBES, tubeR = INVALID_TUBES;

	const unsigned long microDelta = me->nowMicros - me->lastCallTimeMicros;

	if (antiPoisonEnd > curMillis) {
		const uint16_t sym = getNumber((antiPoisonEnd - curMillis) / ANTI_POISON_DELAY);
		tubeL = sym;
		tubeR = sym;
		analogWrite(PIN_LED_RED, 0);
		analogWrite(PIN_LED_GREEN, 0);
		analogWrite(PIN_LED_BLUE, 0);
	}
	else if (displayUpdateTask) {
		if (displayUpdateTask->render(microDelta)) {
			displayTriggerEffects();
		}
		analogWrite(PIN_LED_RED, displayUpdateTask->red);
		analogWrite(PIN_LED_GREEN, displayUpdateTask->green);
		analogWrite(PIN_LED_BLUE, displayUpdateTask->blue);
	}

	displayEffectsUpdate(microDelta);

	const byte curTubeL = anodeGroup << 1;
	const byte curTubeR = curTubeL + 1;

	if (tubeL == INVALID_TUBES) {
		tubeL = dataToDisplay[curTubeL];
#ifdef EFFECT_ENABLED
		byte tubeTrans = dataIsTransitioning[curTubeL];
		if (tubeTrans > 0) {
#ifdef EFFECT_SLOT_MACHINE
			tubeL = getNumber(tubeTrans / (EFFECT_SPEED / 10));
#endif
		}
#endif
	}

	if (tubeR == INVALID_TUBES) {
		tubeR = dataToDisplay[curTubeR];
#ifdef EFFECT_ENABLED
		byte tubeTrans = dataIsTransitioning[curTubeR];
		if (tubeTrans > 0) {
#ifdef EFFECT_SLOT_MACHINE
			tubeR = getNumber(tubeTrans / (EFFECT_SPEED / 10));
#endif
		}
#endif
	}

	digitalWrite(PIN_DISPLAY_LATCH, LOW);
	SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE2));
	SPI.transfer(dotMask);                            // [   ][   ][   ][   ][   ][   ][L1 ][L0 ] - L0     L1 - dots
	SPI.transfer(tubeR >> 6 | 1 << (anodeGroup + 4)); // [   ][A2 ][A1 ][A0 ][RC9][RC8][RC7][RC6] - A0  -  A2 - anodes
	SPI.transfer(tubeR << 2 | tubeL >> 8);            // [RC5][RC4][RC3][RC2][RC1][RC0][LC9][LC8] - RC9 - RC0 - Right tubes cathodes
	SPI.transfer(tubeL);                              // [LC7][LC6][LC5][LC4][LC3][LC2][LC1][LC0] - LC9 - LC0 - Left tubes cathodes
	SPI.endTransaction();
	digitalWrite(PIN_DISPLAY_LATCH, HIGH);

	if (++anodeGroup > 2) {
		anodeGroup = 0;
	}
}

