#pragma once
#include "../wut_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void OSReport(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
