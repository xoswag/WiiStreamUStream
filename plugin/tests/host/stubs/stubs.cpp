// Host implementations of the coreinit / WHB calls the encoder uses.
#include "../test_hooks.hpp"

#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point kStart = Clock::now();
// A real console clock reads in the tens of quadrillions of ticks; starting there
// catches any code that truncates an absolute time to 32 bits.
constexpr OSTime kEpochTicks = 10000000000000000LL;

thread_local int tCore = -1;

struct Starve {
    std::atomic<int> percent{0};
    std::atomic<int> minMs{0};
    std::atomic<int> maxMs{0};
};
Starve sStarve[3];

void maybeStarve() {
    if (tCore < 0 || tCore > 2) {
        return;
    }
    const int pct = sStarve[tCore].percent.load();
    if (pct <= 0) {
        return;
    }
    thread_local std::mt19937 rng(12345u + (unsigned) tCore);
    if ((int) (rng() % 100) >= pct) {
        return;
    }
    const int lo = sStarve[tCore].minMs.load();
    const int hi = sStarve[tCore].maxMs.load();
    const int ms = lo + (hi > lo ? (int) (rng() % (unsigned) (hi - lo + 1)) : 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct QueueImpl {
    std::mutex m;
    std::condition_variable cv;
    std::deque<OSMessage> q;
    size_t cap = 0;
};
std::mutex sQueuesMutex;
std::vector<std::unique_ptr<QueueImpl>> sQueues; // never freed mid-run: a reset queue may still be referenced

struct ThreadImpl {
    OSThreadEntryPointFn entry = nullptr;
    int argc                   = 0;
    char *argv                 = nullptr;
    int core                   = -1;
    int result                 = 0;
    std::thread th;
};

std::mutex sLogMutex;
std::map<std::string, int> sLogCounts;
const char *const kTracked[] = {"Benching core", "rejoins", "falling back to blocking", "##ERROR##", "##WARN ##",
                                "[fps]", "[cost]", "[cores]"};

std::atomic<int> sBadCacheOps{0};

void recordLog(const char *line) {
    std::lock_guard<std::mutex> l(sLogMutex);
    for (const char *t : kTracked) {
        if (strstr(line, t) != nullptr) {
            sLogCounts[t]++;
        }
    }
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void checkCacheOp(const char *what, void *addr, uint32_t size) {
    if (addr == nullptr || size == 0 || ((uintptr_t) addr & 31) != 0 || (size & 31) != 0) {
        fprintf(stdout, "CACHE-OP %s(%p, %u): not whole 32-byte lines - on hardware this would "
                        "touch a neighbour's cache line\n",
                what, addr, size);
        sBadCacheOps++;
        return;
    }
    // Touch both ends so AddressSanitizer flags a range that runs outside its buffer.
    volatile uint8_t first = ((uint8_t *) addr)[0];
    volatile uint8_t last  = ((uint8_t *) addr)[size - 1];
    (void) first;
    (void) last;
}

} // namespace

// --- test hooks ----------------------------------------------------------------

namespace testhooks {

void setStarve(int core, int percent, int minMs, int maxMs) {
    sStarve[core].minMs   = minMs;
    sStarve[core].maxMs   = maxMs;
    sStarve[core].percent = percent;
}

void clearStarve() {
    for (auto &s : sStarve) {
        s.percent = 0;
    }
}

int logCount(const std::string &needle) {
    std::lock_guard<std::mutex> l(sLogMutex);
    auto it = sLogCounts.find(needle);
    return it == sLogCounts.end() ? 0 : it->second;
}

void resetLogCounts() {
    std::lock_guard<std::mutex> l(sLogMutex);
    sLogCounts.clear();
}

int badCacheOps() {
    return sBadCacheOps.load();
}

} // namespace testhooks

// --- coreinit ------------------------------------------------------------------

extern "C" {

OSTime OSGetSystemTime(void) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - kStart).count();
    return kEpochTicks + (OSTime) ((__int128) ns * (__int128) OSTimerClockSpeed / 1000000000);
}

OSTime OSGetTime(void) {
    return OSGetSystemTime();
}

void DCInvalidateRange(void *addr, uint32_t size) {
    checkCacheOp("DCInvalidateRange", addr, size);
}

void DCFlushRange(void *addr, uint32_t size) {
    checkCacheOp("DCFlushRange", addr, size);
}

void OSMemoryBarrier(void) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void OSInitMessageQueue(OSMessageQueue *queue, OSMessage * /*messages*/, int32_t size) {
    auto impl  = std::make_unique<QueueImpl>();
    impl->cap  = (size_t) size;
    queue->impl = impl.get();
    std::lock_guard<std::mutex> l(sQueuesMutex);
    sQueues.push_back(std::move(impl));
}

BOOL OSSendMessage(OSMessageQueue *queue, OSMessage *message, OSMessageFlags flags) {
    auto *q = (QueueImpl *) queue->impl;
    std::unique_lock<std::mutex> l(q->m);
    if (q->q.size() >= q->cap) {
        if (!(flags & OS_MESSAGE_FLAGS_BLOCKING)) {
            return FALSE;
        }
        q->cv.wait(l, [q] { return q->q.size() < q->cap; });
    }
    q->q.push_back(*message);
    q->cv.notify_all();
    return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue *queue, OSMessage *message, OSMessageFlags flags) {
    auto *q = (QueueImpl *) queue->impl;
    {
        std::unique_lock<std::mutex> l(q->m);
        if (q->q.empty()) {
            if (!(flags & OS_MESSAGE_FLAGS_BLOCKING)) {
                return FALSE;
            }
            q->cv.wait(l, [q] { return !q->q.empty(); });
        }
        *message = q->q.front();
        q->q.pop_front();
        q->cv.notify_all();
    }
    if (flags & OS_MESSAGE_FLAGS_BLOCKING) {
        maybeStarve(); // woken, but the game has the core
    }
    return TRUE;
}

BOOL OSCreateThread(OSThread *thread, OSThreadEntryPointFn entry, int32_t argc, char *argv, void * /*stack*/,
                    uint32_t /*stackSize*/, int32_t /*priority*/, OSThreadAttributes attributes) {
    auto *t  = new ThreadImpl;
    t->entry = entry;
    t->argc  = argc;
    t->argv  = argv;
    for (int c = 0; c < 3; c++) {
        if (attributes & (1 << c)) {
            t->core = c;
            break;
        }
    }
    thread->impl = t;
    return TRUE;
}

int32_t OSResumeThread(OSThread *thread) {
    auto *t = (ThreadImpl *) thread->impl;
    t->th   = std::thread([t] {
        tCore     = t->core;
        t->result = t->entry(t->argc, (const char **) t->argv);
    });
    return 0;
}

BOOL OSJoinThread(OSThread *thread, int *threadResult) {
    auto *t = (ThreadImpl *) thread->impl;
    t->th.join();
    if (threadResult != nullptr) {
        *threadResult = t->result;
    }
    delete t;
    thread->impl = nullptr;
    return TRUE;
}

void OSSetThreadName(OSThread * /*thread*/, const char * /*name*/) {
}

void OSSleepTicks(OSTime ticks) {
    if (ticks <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds((int64_t) ((__int128) ticks * 1000000000 / OSTimerClockSpeed)));
}

void OSReport(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n > 0 && buf[n - 1] == '\n') {
        buf[--n] = '\0';
    }
    recordLog(buf);
}

BOOL WHBLogPrintf(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    recordLog(buf);
    return TRUE;
}

} // extern "C"
