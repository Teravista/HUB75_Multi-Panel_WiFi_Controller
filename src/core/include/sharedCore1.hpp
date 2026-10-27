#pragma once
#include "core/include/Core1SchedulerClass.hpp"

// Returns the scheduler shared by network work and JPEG decoding.
Core1Scheduler& SharedCore1Scheduler(void);
