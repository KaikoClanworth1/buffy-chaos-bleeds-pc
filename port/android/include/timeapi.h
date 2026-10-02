/* timeapi.h on Android: the timer resolution calls are no-ops (POSIX sleeps
 * are already fine-grained). */
#pragma once
#define timeBeginPeriod(ms) ((void)(ms), 0)
#define timeEndPeriod(ms)   ((void)(ms), 0)
