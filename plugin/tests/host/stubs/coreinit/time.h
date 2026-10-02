// Host-test stand-in for <coreinit/time.h>. Same macro shapes as wut, with the
// console's timer rate (bus clock / 4), so tick arithmetic overflows or truncates
// here exactly where it would on hardware.
#pragma once
#include "../wut_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t OSTime;
typedef int32_t OSTick;

#define OSTimerClockSpeed 62156250ull

#define OSSecondsToTicks(val)      ((uint64_t) (val) * (uint64_t) OSTimerClockSpeed)
#define OSMillisecondsToTicks(val) (((uint64_t) (val) * (uint64_t) OSTimerClockSpeed) / 1000ull)
#define OSMicrosecondsToTicks(val) (((uint64_t) (val) * (uint64_t) OSTimerClockSpeed) / 1000000ull)
#define OSTicksToSeconds(val)      ((uint64_t) (val) / (uint64_t) OSTimerClockSpeed)
#define OSTicksToMilliseconds(val) (((uint64_t) (val) * 1000ull) / (uint64_t) OSTimerClockSpeed)
#define OSTicksToMicroseconds(val) (((uint64_t) (val) * 1000000ull) / (uint64_t) OSTimerClockSpeed)

OSTime OSGetTime(void);
OSTime OSGetSystemTime(void);

#ifdef __cplusplus
}
#endif
