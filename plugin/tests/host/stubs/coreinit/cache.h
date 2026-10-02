// Host-test stand-in for <coreinit/cache.h>. The range calls check that they
// were given whole, in-bounds cache lines (see stubs.cpp).
#pragma once
#include "../wut_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void DCInvalidateRange(void *addr, uint32_t size);
void DCFlushRange(void *addr, uint32_t size);
void OSMemoryBarrier(void);

#ifdef __cplusplus
}
#endif
