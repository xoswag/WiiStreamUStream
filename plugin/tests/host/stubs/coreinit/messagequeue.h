// Host-test stand-in for <coreinit/messagequeue.h>: bounded FIFO, blocking or not,
// same as Cafe OS. A blocking receive on a "slow core" may be delayed (stubs.cpp).
#pragma once
#include "../wut_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OSMessage {
    void *message;
    uint32_t args[3];
} OSMessage;

typedef struct OSMessageQueue {
    void *impl;
} OSMessageQueue;

typedef enum OSMessageFlags {
    OS_MESSAGE_FLAGS_NONE     = 0,
    OS_MESSAGE_FLAGS_BLOCKING = 1 << 0,
} OSMessageFlags;

void OSInitMessageQueue(OSMessageQueue *queue, OSMessage *messages, int32_t size);
BOOL OSSendMessage(OSMessageQueue *queue, OSMessage *message, OSMessageFlags flags);
BOOL OSReceiveMessage(OSMessageQueue *queue, OSMessage *message, OSMessageFlags flags);

#ifdef __cplusplus
}
#endif
