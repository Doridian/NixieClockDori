#ifndef _DISPLAYTASK_COUNTDOWN_H_INCLUDED
#define _DISPLAYTASK_COUNTDOWN_H_INCLUDED

#include "DisplayTask.h"

class DisplayTask_Countdown : public DisplayTask {
public:
	bool render(const Task* renderTask) override;
	unsigned long to;
protected:
	const bool _canShow() override;
};

#endif
