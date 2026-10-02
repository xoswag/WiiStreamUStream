// Host-test stand-in for <coreinit/thread.h>, backed by std::thread. Affinity is
// remembered so tests can make one "core" slow, the way a busy game does.
#pragma once
#include "../wut_types.h"
#include "time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*OSThreadEntryPointFn)(int argc, const char **argv);
typedef uint8_t OSThreadAttributes;

typedef struct OSThread {
    void *impl;
    uint8_t opaque[0x40];
} OSThread;

BOOL OSCreateThread(OSThread *thread, OSThreadEntryPointFn entry, int32_t argc, char *argv, void *stack,
                    uint32_t stackSize, int32_t priority, OSThreadAttributes attributes);
int32_t OSResumeThread(OSThread *thread);
BOOL OSJoinThread(OSThread *thread, int *threadResult);
void OSSetThreadName(OSThread *thread, const char *name);
void OSSleepTicks(OSTime ticks);

#ifdef __cplusplus
}
#endif
