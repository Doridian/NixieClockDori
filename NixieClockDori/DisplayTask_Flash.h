#pragma once

#include "DisplayTask.h"

class DisplayTask_Flash : public DisplayTask {
public:
	DisplayTask_Flash();

	bool refresh() override;

	unsigned long endTime = 0;
	unsigned long lastUpdate = 0;
	bool allowEffects = true;

	void setDataFromSerial();

protected:
	bool _canShow() const override;
};
