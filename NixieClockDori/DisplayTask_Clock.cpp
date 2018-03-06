#include "DisplayTask_Clock.h"
#include "Display.h"
#include "config.h"
#include "rtc.h"

#include <TimeLib.h>

void DisplayTask_Clock::handleButtonPress(Button button, PressType pressType) {
	if ((button == UP || button == DOWN) && pressType == Click && !this->editMode) {
		currentEffect = static_cast<DisplayEffect>(static_cast<byte>(currentEffect) + 1);
		if (currentEffect == FIRST_INVALID) {
			currentEffect = NONE;
		}
		return;
	}
	DisplayTask::handleButtonPress(button, pressType);
}

void DisplayTask_Clock::handleEdit(byte digit, bool up) {
	switch (digit) {
	case 0:
		if (up) {
			if (h >= 20 || (h >= 10 && (h % 10) > 3)) {
				h %= 10;
			}
			else {
				h += 10;
			}
		}
		else {
			if (h < 10) {
				h = (((h % 10) > 3) ? 10 : 20) + (h % 10);
			}
			else {
				h -= 10;
			}
		}
		break;
	case 1:
		if (up) {
			if ((h % 10) == 9 || ((h % 10) >= 3 && h >= 20)) {
				h -= h % 10;
			}
			else {
				h += 1;
			}
		}
		else {
			if ((h % 10) == 0) {
				h -= (h % 10) - ((h >= 20) ? 3 : 9);
			}
			else {
				h -= 1;
			}
		}
		break;
	case 2:
		if (up) {
			if (m >= 50) {
				m %= 10;
			}
			else {
				m += 10;
			}
		}
		else {
			if (m < 10) {
				m = 50 + m % 10;
			}
			else {
				m -= 10;
			}
		}
		break;
	case 3:
		if (up) {
			if ((m % 10) == 9) {
				m -= 9;
			}
			else {
				m += 1;
			}
		}
		else {
			if ((m % 10) == 0) {
				m += 9;
			}
			else {
				m -= 1;
			}
		}
		break;
	case 4:
		if (up) {
			if (s >= 50) {
				s %= 10;
			}
			else {
				s += 10;
			}
		}
		else {
			if (s < 10) {
				s = 50 + s % 10;
			}
			else {
				s -= 10;
			}
		}
		break;
	case 5:
		if (up) {
			if ((s % 10) == 9) {
				s -= 9;
			}
			else {
				s += 1;
			}
		}
		else {
			if ((s % 10) == 0) {
				s += 9;
			}
			else {
				s -= 1;
			}
		}
		break;
	}

	tmElements_t tm;
	breakTime(now(), tm);
	tm.Hour = h;
	tm.Minute = m;
	tm.Second = s;
	rtcSetTime(tm);
}

bool DisplayTask_Clock::refresh(uint16_t displayData[]) {
	if (!DisplayTask::editMode) {
		const time_t _n = now();
		const byte h = hour(_n);
		const byte m = minute(_n);
		const byte s = second(_n);

		if (h < 4 && s != this->s && s % 5 == 2) {
			displayAntiPoison(1);
		}
		else if (m != this->m && m % 10 == 2) {
			displayAntiPoison(2);
		}

		this->h = h;
		this->m = m;
		this->s = s;
	}

	if (this->s % 2 || DisplayTask::editMode) {
		this->dotMask = makeDotMask(true, true);
	}
	else {
		this->dotMask = makeDotMask(false, false);
	}

#ifdef CLOCK_TRIM_HOURS
	insert1(0, this->h / 10, true, displayData);
	insert1(1, this->h, false, displayData);
#else
	insert2(0, this->h, false, displayData);
#endif
	insert2(2, this->m, false, displayData);
	insert2(4, this->s, false, displayData);

	return DisplayTask::refresh(displayData);
}

