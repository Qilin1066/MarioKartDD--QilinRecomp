// Threads, mutexes, message queues and alarms (dolphin/os's OSThread,
// OSMutex, OSMessage, OSAlarm), natively.
//
// Each guest thread runs on a host thread of its own, with its own registers
// (a Cpu), and exactly one of them runs at a time: the scheduler hands the
// guest CPU over where the guest OS would switch, namely when the running thread
// blocks or yields, or when it wakes a thread of higher priority. Interrupts
// arrive at safe points (os.hpp), so preemption by an interrupt's wake-up
// happens there too. The rules are the OS's: priorities 0 (highest) to 31,
// first come first served within one, suspend counts, threads created
// suspended.
//
// Guest structures stay up to date where the game can read them: an OSThread's
// state, suspend count, priority and exit value, the current thread at
// 0x800000E4 and its context at 0x800000D4, the list of active threads from
// 0x800000DC, a mutex's owner and count, a message queue's ring. Who waits on
// what is kept here, by the guest address of the queue waited on.
#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <cstdio>
#include <map>
#include <string>
#include <set>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX  // MinGW defines it already
#define NOMINMAX  // windows.h's min and max macros would break std::min
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "boot.hpp"
#include "build.hpp"
#include "os.hpp"
#include "threads.hpp"
#include "priority.hpp"
#include "qilin/rt/report.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// OSThread (0x318 bytes), as OSCreateThread (800EB780) and __OSThreadInit
// (800EB010) fill it: its OSContext (0x2C8 bytes), then these.
constexpr uint32_t kState = 0x2C8, kAttr = 0x2CA, kSuspend = 0x2CC, kPriority = 0x2D0, kBase = 0x2D4, kVal = 0x2D8,
                   kQueue = 0x2DC, kQueueJoin = 0x2E8, kActiveNext = 0x2FC, kActivePrev = 0x300, kStackBase = 0x304,
                   kStackEnd = 0x308;
enum : uint16_t { kReady = 1, kRunning = 2, kWaiting = 4, kMoribund = 8 };
constexpr uint32_t kCurrentThread = 0x800000E4u, kDefaultThreadPtr = 0x800000D8u;
constexpr uint32_t kCurrentContext = 0x800000D4u, kCurrentContextPhysical = 0x800000C0u;
constexpr uint32_t kActiveHead = 0x800000DCu, kActiveTail = 0x800000E0u;
constexpr uint32_t kDefaultThread = build::kDefaultThread;
constexpr uint32_t kOSExitThread = build::kOSExitThread;
constexpr uint32_t kFpeHandler = build::kErrorTable + 4 * 16;  // OSCreateThread fills FPRs with -1 when one is set
constexpr size_t kHostStack = 16u << 20;

struct ThreadCancelled {};

struct ThreadExit {
    uint32_t value;
};

struct Thread {
    uint32_t guest = 0;
    Cpu* cpu = nullptr;          // the main thread's lives in start_game
    std::unique_ptr<Cpu> own;    // other threads' here
    int priority = 16;           // the one it runs at: its own, or a higher one lent by a mutex's waiter
    int wanted_priority = 16;
    int base = 16;               // its own
    int suspend = 0;
    uint16_t state = kReady;
    bool detached = false;
    bool enabled = true;         // its interrupt-enable flag while it isn't running
    uint32_t waiting_on = 0;     // the queue it sleeps on, or 0
    uint32_t entry = 0;
    bool started = false, go = false;
    bool cancelled = false;  // under g_lock: wake a parked host so it can unwind
    std::condition_variable cv;
    std::vector<uint32_t> mutexes;  // held, for OSExitThread's unlock
    uint64_t ran_ns = 0;            // the time it has had the guest CPU, less idle waits (log_threads)
};

std::mutex g_lock;  // guards the hand-over (go flags, condition variables)
std::map<uint32_t, std::shared_ptr<Thread>> g_threads;
Thread* g_current = nullptr;
std::deque<Thread*> g_ready[32];
std::map<uint32_t, std::deque<Thread*>> g_waiters;
std::set<uint32_t> g_owned;  // the mutexes that have an owner
int g_scheduler_off = 0;     // OSDisableScheduler's count: no switch while it's above 0

// The OS's list of active threads (__OSActiveThreadQueue), through each
// thread's link at +0x2FC: appended on creation, left on exit once detached.
void active_add(uint32_t t) {
    const uint32_t tail = rt::peek32(kActiveTail);
    if (tail) rt::poke32(tail + kActiveNext, t);
    else rt::poke32(kActiveHead, t);
    rt::poke32(t + kActivePrev, tail);
    rt::poke32(t + kActiveNext, 0);
    rt::poke32(kActiveTail, t);
}
void active_remove(uint32_t t) {
    const uint32_t next = rt::peek32(t + kActiveNext), prev = rt::peek32(t + kActivePrev);
    if (next) rt::poke32(next + kActivePrev, prev);
    else rt::poke32(kActiveTail, prev);
    if (prev) rt::poke32(prev + kActiveNext, next);
    else rt::poke32(kActiveHead, next);
}

void store(Thread* t) {
    // A finished thread reads as moribund until it's detached, then as 0.
    rt::poke16(t->guest + kState, t->state == kMoribund && t->detached ? 0 : t->state);
    rt::poke32(t->guest + kSuspend, static_cast<uint32_t>(t->suspend));
    rt::poke32(t->guest + kPriority, static_cast<uint32_t>(t->priority));
    rt::poke32(t->guest + kQueue, t->waiting_on);
}

Thread* thread_at(Cpu& c, uint32_t guest) {
    const auto it = g_threads.find(guest);
    if (it == g_threads.end()) rt::fatal(&c, "%08X isn't a thread OSCreateThread made", guest);
    return it->second.get();
}

void make_ready(Thread* t) {
    t->state = kReady;
    t->waiting_on = 0;
    if (t->suspend == 0) g_ready[t->priority].push_back(t);
    store(t);
}
void unready(Thread* t) {
    auto& q = g_ready[t->priority];
    for (auto it = q.begin(); it != q.end(); ++it)
        if (*it == t) {
            q.erase(it);
            return;
        }
}
Thread* best_ready() {
    for (auto& q : g_ready)
        if (!q.empty()) return q.front();
    return nullptr;
}

void host_main(Thread* t);
void recompute_priorities();

void start_host(Thread* t) {
    t->started = true;
    // A cancelled guest address can be reused before its old host has unwound.
    // The host keeps its own reference until it has stopped touching the thread.
    auto* lifetime = new std::shared_ptr<Thread>(g_threads.at(t->guest));
    run_on_host_thread([](void* p) {
        std::shared_ptr<Thread> keep = std::move(*static_cast<std::shared_ptr<Thread>*>(p));
        delete static_cast<std::shared_ptr<Thread>*>(p);
        host_main(keep.get());
    }, lifetime);
}

// The running thread's time so far goes to its account (for log_threads).
std::chrono::steady_clock::time_point g_running_since = std::chrono::steady_clock::now();
uint64_t g_idle_then = 0;
void account(Thread* t) {
    const auto now = std::chrono::steady_clock::now();
    const uint64_t idle = idle_time_ns(), wall = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_running_since).count());
    t->ran_ns += wall - std::min(wall, idle - g_idle_then);
    g_running_since = now, g_idle_then = idle;
}

// Hand the guest CPU to `next`; unless `leaving`, wait until it comes back.
void switch_to(Thread* next, bool leaving = false) {
    // Keep the notified condition variable alive even if the other host
    // finishes and its guest address is immediately reused before we park.
    const auto keep_next = g_threads.at(next->guest);
    next = keep_next.get();
    Thread* me = g_current;
    account(me);
    me->enabled = interrupts_enabled();
    unready(next);
    next->state = kRunning;
    store(next);
    g_current = next;
    rt::poke32(kCurrentThread, next->guest);
    rt::poke32(kCurrentContext, next->guest);  // its OSContext is its first member
    rt::poke32(kCurrentContextPhysical, next->guest & 0x3FFFFFFFu);
    if (!next->started) start_host(next);
    {
        std::lock_guard l(g_lock);
        next->go = true;
    }
    next->cv.notify_one();
    if (leaving) return;
    std::unique_lock l(g_lock);
    me->cv.wait(l, [me] { return me->go || me->cancelled; });
    if (me->cancelled) throw ThreadCancelled{};
    me->go = false;
    l.unlock();
    set_interrupts_enabled(me->enabled);
}

// The running thread can't go on (it waits, is suspended, or has exited): run
// another, or, with none ready, act as the OS's idle loop until an interrupt
// readies one. Returns when this thread runs again (never if `leaving`).
void block(Cpu& c, bool leaving = false) {
    Thread* me = g_current;
    for (;;) {
        if (Thread* next = best_ready()) {
            if (next == me) {  // woken while still on its own host thread
                unready(me);
                me->state = kRunning;
                store(me);
                return;
            }
            switch_to(next, leaving);
            return;
        }
        if (deliver_interrupts_idle(c)) continue;
        const uint64_t deadline = next_timer_deadline();
        if (deadline == UINT64_MAX && !has_pending_interrupts())
            rt::fatal(&c, "every guest thread is waiting, and nothing is left to wake one (deadlock)");
        const uint64_t now = time_base();
        if (deadline > now)
            idle_sleep((deadline - now) * 1000000000 / kTimerHz);
    }
}

void host_main(Thread* t) {
    {
        std::unique_lock l(g_lock);
        t->cv.wait(l, [t] { return t->go || t->cancelled; });
        if (t->cancelled) return;
        t->go = false;
    }
    set_interrupts_enabled(true);  // a new thread's context has interrupts on
    rt::set_thread_cpu(t->cpu);    // for a crash report's call stack
    uint32_t value;
    try {
        rt::call(*t->cpu, t->entry);
        value = t->cpu->r[3];
    } catch (const ThreadCancelled&) {
        return;  // cancellation already released locks and handed on the guest CPU
    } catch (const ThreadExit& e) {
        value = e.value;
    }
    finish_thread(*t->cpu, value);
}

// Ready every thread sleeping on `queue`, without switching. True if any.
bool wake_waiters(uint32_t queue) {
    const auto it = g_waiters.find(queue);
    if (it == g_waiters.end() || it->second.empty()) return false;
    std::deque<Thread*> woken = std::move(it->second);
    g_waiters.erase(it);
    for (Thread* t : woken)
        if (t->state == kWaiting) make_ready(t);
    if (g_owned.contains(queue)) recompute_priorities();
    return true;
}

void sleep_on(Cpu& c, uint32_t queue) {
    Thread* me = g_current;
    me->state = kWaiting;
    me->waiting_on = queue;
    store(me);
    g_waiters[queue].push_back(me);
    if (g_owned.contains(queue)) recompute_priorities();
    block(c);
}

// Switch to a thread readied by the running one, if it outranks it.
void reschedule(Cpu& c) {
    if (in_interrupt() || g_scheduler_off > 0) return;  // the interrupt's end switches (preempt)
    Thread* me = g_current;
    Thread* best = best_ready();
    if (best && best->priority < me->priority) {
        make_ready(me);
        switch_to(best);
    }
    (void)c;
}

// ---- alarms: by guest address; an OSAlarm keeps its handler at +0, the time
// it fires at +8, its period at +0x18 and a periodic alarm's start at +0x20
// (OSSetAlarm, 800E667C; OSSetPeriodicAlarm, 800E66E4), for code that reads them.
struct Alarm {
    uint64_t fire = 0, period = 0;
    uint32_t handler = 0;
    uint64_t generation = 0;
};
std::map<uint32_t, Alarm> g_alarms;
uint64_t g_generation = 0;

void poll_alarms() {
    const uint64_t now = time_base();
    for (auto& [a, al] : g_alarms) {
        if (al.handler == 0 || al.fire > now) continue;
        const uint64_t gen = al.generation;
        const uint32_t handler = al.handler, alarm = a;
        if (al.period) {
            al.fire += al.period * ((now - al.fire) / al.period + 1);
            rt::poke32(a + 8, static_cast<uint32_t>(al.fire >> 32));
            rt::poke32(a + 12, static_cast<uint32_t>(al.fire));
        } else {
            al.handler = 0;  // one-shot: disarmed before its handler runs
        }
        post_interrupt([alarm, handler, gen](Cpu& c) {
            const auto it = g_alarms.find(alarm);
            if (it == g_alarms.end() || it->second.generation != gen) return;  // cancelled or set again since
            if (!it->second.period) rt::poke32(alarm, 0);
            guest_callback(c, handler, alarm, g_current ? g_current->guest : 0);
        });
    }
}
uint64_t alarm_deadline() {
    uint64_t t = UINT64_MAX;
    for (const auto& [a, al] : g_alarms)
        if (al.handler) t = std::min(t, al.fire);
    return t;
}

void arm(uint32_t a, uint64_t fire, uint64_t period, uint32_t handler) {
    Alarm& al = g_alarms[a];
    al = {fire, period, handler, ++g_generation};
    rt::poke32(a, handler);
    rt::poke32(a + 8, static_cast<uint32_t>(fire >> 32));
    rt::poke32(a + 12, static_cast<uint32_t>(fire));
    rt::poke32(a + 0x18, static_cast<uint32_t>(period >> 32));
    rt::poke32(a + 0x1C, static_cast<uint32_t>(period));
}
uint64_t arg64(const Cpu& c, unsigned r) { return uint64_t{c.r[r]} << 32 | c.r[r + 1]; }

// ---- the API: threads
void OSGetCurrentThread(Cpu& c) { c.r[3] = g_current->guest; }

void OSCreateThread(Cpu& c) {  // (thread, func, param, stack, stackSize, priority, attr) -> BOOL
    const uint32_t t = c.r[3], func = c.r[4], param = c.r[5], stack = c.r[6], size = c.r[7], attr = c.r[9];
    const int32_t prio = static_cast<int32_t>(c.r[8]);
    if (prio < 0 || prio > 31) {
        c.r[3] = 0;
        return;
    }
    if (auto it = g_threads.find(t); it != g_threads.end()) {
        if (it->second->state != kMoribund)
            rt::fatal(&c, "OSCreateThread on %08X, a thread that is still alive", t);
        if (!it->second->detached) active_remove(t);  // never joined or detached: the OS would leave it listed
        g_threads.erase(it);
    }
    auto th = std::make_shared<Thread>();
    th->guest = t;
    th->own = std::make_unique<Cpu>(rt::new_cpu());
    th->cpu = th->own.get();
    th->priority = th->base = prio;
    th->suspend = 1;
    th->state = kReady;
    th->detached = attr & 1;
    th->entry = func;
    Cpu& n = *th->cpu;
    const uint32_t top = stack & ~7u;
    rt::poke32(top - 8, 0);
    rt::poke32(top - 4, 0);
    n.r[1] = top - 8;
    n.r[2] = c.r[2];
    n.r[13] = c.r[13];
    n.r[3] = param;
    n.lr = kOSExitThread;
    if (rt::peek32(kFpeHandler) != 0)
        for (int i = 0; i < 32; ++i) n.ps0[i] = n.ps1[i] = std::bit_cast<double>(~uint64_t{0});
    // The guest's OSThread, as OSCreateThread fills it.
    std::memset(rt::guest_ptr(t + kState), 0, 0x318 - kState);
    rt::poke16(t + kAttr, static_cast<uint16_t>(attr & 1));
    rt::poke32(t + kBase, static_cast<uint32_t>(prio));
    rt::poke32(t + kVal, 0xFFFFFFFFu);
    rt::poke32(t + kStackBase, stack);
    rt::poke32(t + kStackEnd, stack - size);
    rt::poke32(stack - size, 0xDEADBABEu);
    Thread* p = th.get();
    g_threads[t] = std::move(th);
    store(p);
    active_add(t);
    c.r[3] = 1;
}

void OSResumeThread(Cpu& c) {  // (thread) -> the suspend count before
    Thread* t = thread_at(c, c.r[3]);
    const int was = t->suspend;
    if (t->suspend > 0 && --t->suspend == 0 && t->state == kReady) make_ready(t);
    store(t);
    recompute_priorities();
    c.r[3] = static_cast<uint32_t>(was);
    reschedule(c);
}

void OSSuspendThread(Cpu& c) {  // (thread) -> the suspend count before
    Thread* t = thread_at(c, c.r[3]);
    const int was = t->suspend++;
    c.r[3] = static_cast<uint32_t>(was);
    if (was == 0) {
        if (t == g_current) {
            t->state = kReady;  // ready, but held until resumed
            store(t);
            recompute_priorities();
            block(c);
            c.r[3] = static_cast<uint32_t>(was);
            return;
        }
        if (t->state == kReady) unready(t);
    }
    store(t);
    recompute_priorities();
    reschedule(c);
}

void OSYieldThread(Cpu& c) {
    // A yield is how the guest waits for others in a loop: a safe point, so an
    // interrupt (a read finishing, the retrace) can arrive, as it would mid-loop.
    deliver_interrupts(c);
    Thread* me = g_current;
    Thread* best = best_ready();
    if (best && best->priority <= me->priority && g_scheduler_off <= 0) {
        make_ready(me);
        switch_to(best);
    }
}

void OSExitThread(Cpu& c) {
    if (g_current->guest == kDefaultThread) rt::fatal(&c, "the main thread called OSExitThread");
    throw ThreadExit{c.r[3]};
}

void release_mutex(Thread* me, uint32_t m);

void OSCancelThread(Cpu& c) {
    Thread* t = thread_at(c, c.r[3]);
    if (t == g_current) throw ThreadExit{0};
    if (t->state == kReady) unready(t);
    if (t->waiting_on) {
        auto& q = g_waiters[t->waiting_on];
        for (auto it = q.begin(); it != q.end(); ++it)
            if (*it == t) {
                q.erase(it);
                break;
            }
    }
    // Unlock and wake without switching until cancellation is fully recorded.
    for (uint32_t m : std::vector(t->mutexes)) {
        release_mutex(t, m);
        wake_waiters(m);
    }
    t->state = kMoribund;
    t->waiting_on = 0;
    if (t->detached) active_remove(t->guest);
    store(t);
    {
        std::lock_guard lock(g_lock);
        t->cancelled = true;
    }
    t->cv.notify_one();
    wake_waiters(t->guest + kQueueJoin);
    recompute_priorities();
    reschedule(c);
}

void OSDetachThread(Cpu& c) {  // nobody will join it: a finished one leaves the active list now
    Thread* t = thread_at(c, c.r[3]);
    const bool was = t->detached;
    t->detached = true;
    rt::poke16(t->guest + kAttr, rt::peek16(t->guest + kAttr) | 1);
    if (t->state == kMoribund && !was) active_remove(t->guest);
    store(t);
    wake_all(c, t->guest + kQueueJoin);
}

void OSIsThreadTerminated(Cpu& c) {  // moribund, or finished and detached (0)
    const uint16_t state = rt::peek16(c.r[3] + kState);
    c.r[3] = state == kMoribund || state == 0;
}

// While the count is above 0 the running thread keeps the CPU whatever is
// readied; as on the OS, enabling again doesn't itself switch (the next
// interrupt's end, or the next call that would, does).
void OSDisableScheduler(Cpu& c) { c.r[3] = static_cast<uint32_t>(g_scheduler_off++); }
void OSEnableScheduler(Cpu& c) { c.r[3] = static_cast<uint32_t>(g_scheduler_off--); }

void OSSleepThread(Cpu& c) { sleep_on(c, c.r[3]); }
void OSWakeupThread(Cpu& c) { wake_all(c, c.r[3]); }
void OSInitThreadQueue(Cpu& c) {
    rt::poke32(c.r[3], 0);
    rt::poke32(c.r[3] + 4, 0);
}

// ---- mutexes: queue (+0), owner (+8), count (+0xC).
// Priority inheritance, as the OS's (__OSPromoteThread, __OSGetEffectivePriority): a thread
// that blocks on a mutex lends its priority to the owner, and on through whatever mutex the
// owner is itself waiting for, so a thread in between can't keep the owner (and with it the
// waiter) off the CPU; releasing a mutex takes the owner back to its own priority, or the
// best still lent to it through the mutexes it keeps.


void set_priority(Thread* t, int priority) {
    if (t->priority == priority) return;
    const bool queued = t->state == kReady && t->suspend == 0;
    if (queued) unready(t);
    t->priority = priority;
    if (queued) g_ready[priority].push_back(t);
    store(t);
}
// Rebuild effective priorities from base priorities and current live waiters.
// Starting afresh also withdraws donations after cancellation/suspension, and
// propagates that withdrawal through a chain of mutex owners. No allocation.
void recompute_priorities() {
    for (auto& [address, t] : g_threads) t->wanted_priority = t->base;
    for (size_t pass = 0; pass < g_threads.size(); ++pass) {
        bool changed = false;
        for (const auto& [address, waiter] : g_threads) {
            if (waiter->state != kWaiting || waiter->suspend || !g_owned.contains(waiter->waiting_on)) continue;
            const auto owner = g_threads.find(rt::peek32(waiter->waiting_on + 8));
            if (owner != g_threads.end() && owner->second->wanted_priority > waiter->wanted_priority) {
                owner->second->wanted_priority = waiter->wanted_priority;
                changed = true;
            }
        }
        if (!changed) break;
    }
    for (auto& [address, t] : g_threads) set_priority(t.get(), t->wanted_priority);
}

void OSInitMutex(Cpu& c) {
    std::memset(rt::guest_ptr(c.r[3]), 0, 0x18);
    g_waiters.erase(c.r[3]);
    g_owned.erase(c.r[3]);
}
void lock_mutex(Cpu& c, uint32_t m) {
    Thread* me = g_current;
    for (;;) {
        const uint32_t owner = rt::peek32(m + 8);
        if (owner == 0) {
            rt::poke32(m + 8, me->guest);
            rt::poke32(m + 0xC, 1);
            me->mutexes.push_back(m);
            g_owned.insert(m);
            return;
        }
        if (owner == me->guest) {
            rt::poke32(m + 0xC, rt::peek32(m + 0xC) + 1);
            return;
        }
        sleep_on(c, m);
    }
}
void release_mutex(Thread* me, uint32_t m) {
    rt::poke32(m + 8, 0);
    rt::poke32(m + 0xC, 0);
    std::erase(me->mutexes, m);
    g_owned.erase(m);
    if (me->priority != me->base) recompute_priorities();
}
void OSLockMutex(Cpu& c) { lock_mutex(c, c.r[3]); }
void OSUnlockMutex(Cpu& c) {
    const uint32_t m = c.r[3];
    Thread* me = g_current;
    if (rt::peek32(m + 8) != me->guest) return;  // as the OS: only the owner unlocks
    const uint32_t n = rt::peek32(m + 0xC) - 1;
    if (n) {
        rt::poke32(m + 0xC, n);
        return;
    }
    release_mutex(me, m);
    wake_all(c, m);
}

// ---- message queues: send queue (+0), receive queue (+8), msgArray (+0x10),
// msgCount (+0x14), firstIndex (+0x18), usedCount (+0x1C).
void OSInitMessageQueue(Cpu& c) {  // (mq, msgArray, msgCount)
    const uint32_t mq = c.r[3];
    std::memset(rt::guest_ptr(mq), 0, 0x20);
    rt::poke32(mq + 0x10, c.r[4]);
    rt::poke32(mq + 0x14, c.r[5]);
    g_waiters.erase(mq);
    g_waiters.erase(mq + 8);
}
void OSSendMessage(Cpu& c) {  // (mq, msg, flags) -> BOOL; flags & 1: block
    const uint32_t mq = c.r[3], msg = c.r[4], flags = c.r[5];
    const uint32_t count = rt::peek32(mq + 0x14);
    while (rt::peek32(mq + 0x1C) >= count) {
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sleep_on(c, mq);
    }
    const uint32_t used = rt::peek32(mq + 0x1C);
    rt::poke32(rt::peek32(mq + 0x10) + 4 * ((rt::peek32(mq + 0x18) + used) % count), msg);
    rt::poke32(mq + 0x1C, used + 1);
    c.r[3] = 1;
    wake_all(c, mq + 8);
}
void OSJamMessage(Cpu& c) {  // (mq, msg, flags): to the front
    const uint32_t mq = c.r[3], msg = c.r[4], flags = c.r[5];
    const uint32_t count = rt::peek32(mq + 0x14);
    while (rt::peek32(mq + 0x1C) >= count) {
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sleep_on(c, mq);
    }
    const uint32_t first = (rt::peek32(mq + 0x18) + count - 1) % count;
    rt::poke32(mq + 0x18, first);
    rt::poke32(rt::peek32(mq + 0x10) + 4 * first, msg);
    rt::poke32(mq + 0x1C, rt::peek32(mq + 0x1C) + 1);
    c.r[3] = 1;
    wake_all(c, mq + 8);
}
void OSReceiveMessage(Cpu& c) {  // (mq, msg*, flags) -> BOOL
    const uint32_t mq = c.r[3], out = c.r[4], flags = c.r[5];
    while (rt::peek32(mq + 0x1C) == 0) {
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sleep_on(c, mq + 8);
    }
    const uint32_t first = rt::peek32(mq + 0x18);
    if (out) rt::poke32(out, rt::peek32(rt::peek32(mq + 0x10) + 4 * first));
    rt::poke32(mq + 0x18, (first + 1) % rt::peek32(mq + 0x14));
    rt::poke32(mq + 0x1C, rt::peek32(mq + 0x1C) - 1);
    c.r[3] = 1;
    wake_all(c, mq);
}

// ---- alarms
void OSCreateAlarm(Cpu& c) {
    g_alarms.erase(c.r[3]);
    rt::poke32(c.r[3], 0);
    rt::poke32(c.r[3] + 4, 0);
}
void OSSetAlarm(Cpu& c) { arm(c.r[3], time_base() + arg64(c, 5), 0, c.r[7]); }  // (alarm, ticks, handler)
void OSSetPeriodicAlarm(Cpu& c) {  // (alarm, start, period, handler)
    const uint64_t start = arg64(c, 5), period = arg64(c, 7), now = time_base();
    if (period == 0) rt::fatal(&c, "OSSetPeriodicAlarm with a period of 0");
    const uint64_t fire = start > now ? start : start + period * ((now - start) / period + 1);
    arm(c.r[3], fire, period, c.r[9]);
    rt::poke32(c.r[3] + 0x20, static_cast<uint32_t>(start >> 32));
    rt::poke32(c.r[3] + 0x24, static_cast<uint32_t>(start));
}
void OSCancelAlarm(Cpu& c) {
    g_alarms.erase(c.r[3]);
    rt::poke32(c.r[3], 0);
}

const rt::Provided kReplacements[] = {
    // OSThread.c
    {0x800EB168u, "OSInitThreadQueue", OSInitThreadQueue},
    {0x800EB178u, "OSGetCurrentThread", OSGetCurrentThread},
    {0x800EB184u, "OSIsThreadTerminated", OSIsThreadTerminated},
    {0x800EB1B8u, "OSDisableScheduler", OSDisableScheduler},
    {0x800EB1F8u, "OSEnableScheduler", OSEnableScheduler},
    {0x800EB744u, "OSYieldThread", OSYieldThread},
    {0x800EB780u, "OSCreateThread", OSCreateThread},
    {build::kOSExitThread, "OSExitThread", OSExitThread},
    {0x800EBA4Cu, "OSCancelThread", OSCancelThread},
    {0x800EBC08u, "OSDetachThread", OSDetachThread},
    {0x800EBCA8u, "OSResumeThread", OSResumeThread},
    {0x800EBF30u, "OSSuspendThread", OSSuspendThread},
    {0x800EC0A0u, "OSSleepThread", OSSleepThread},
    {0x800EC18Cu, "OSWakeupThread", OSWakeupThread},
    // OSMutex.c
    {0x800E9680u, "OSInitMutex", OSInitMutex},
    {0x800E96B8u, "OSLockMutex", OSLockMutex},
    {0x800E9794u, "OSUnlockMutex", OSUnlockMutex},
    // OSMessage.c
    {0x800E9000u, "OSInitMessageQueue", OSInitMessageQueue},
    {0x800E9060u, "OSSendMessage", OSSendMessage},
    {0x800E9128u, "OSReceiveMessage", OSReceiveMessage},
    {0x800E9204u, "OSJamMessage", OSJamMessage},
    // OSAlarm.c
    {0x800E641Cu, "OSCreateAlarm", OSCreateAlarm},
    {0x800E667Cu, "OSSetAlarm", OSSetAlarm},
    {0x800E66E4u, "OSSetPeriodicAlarm", OSSetPeriodicAlarm},
    {0x800E6760u, "OSCancelAlarm", OSCancelAlarm},
};

}  // namespace

std::span<const rt::Provided> thread_replacements() { return kReplacements; }

void init_threads(Cpu& main_cpu) {
    auto th = std::make_shared<Thread>();
    th->guest = kDefaultThread;
    th->cpu = &main_cpu;
    th->priority = 16;
    th->state = kRunning;
    th->detached = true;
    th->started = true;
    g_current = th.get();
    // As __OSThreadInit leaves the default thread.
    const uint32_t t = kDefaultThread;
    std::memset(rt::guest_ptr(t + kState), 0, 0x318 - kState);
    rt::poke16(t + kAttr, 1);
    rt::poke32(t + kBase, 16);
    rt::poke32(t + kVal, 0xFFFFFFFFu);
    rt::poke32(t + kStackBase, build::kStackTop);
    rt::poke32(t + kStackEnd, build::kStackEnd);
    rt::poke32(build::kStackEnd, 0xDEADBABEu);
    rt::poke32(kDefaultThreadPtr, t);  // the FPU's owner
    rt::poke32(kCurrentThread, t);
    rt::poke32(kCurrentContext, t);
    rt::poke32(kCurrentContextPhysical, t & 0x3FFFFFFFu);
    rt::poke32(kActiveHead, 0);
    rt::poke32(kActiveTail, 0);
    active_add(t);
    store(th.get());
    g_threads[t] = std::move(th);
    add_timer_source(poll_alarms, alarm_deadline);
}

void log_threads() {
    account(g_current);
    for (const auto& [address, t] : g_threads) {
        std::string stack;
        uint32_t sp = t->cpu->r[1];
        for (int depth = 0; depth < 12 && sp >= 0x80000000u && sp < 0x81800000u - 8; ++depth) {
            const uint32_t next = rt::peek32(sp), lr = rt::peek32(sp + 4);
            if (depth) {  // (the innermost frame's own LR slot isn't written yet)
                char one[16];
                std::snprintf(one, sizeof one, " %08X", lr);
                stack += one;
            }
            if (next <= sp) break;
            sp = next;
        }
        rt::log("thread %08X%s: state %u, priority %d (own %d), suspended %d, sleeps on %08X, entry %08X, ran %.0f ms, lr %08X; stack:%s",
                address, t.get() == g_current ? " (running)" : "", t->state, t->priority, t->base, t->suspend, t->waiting_on, t->entry,
                double(t->ran_ns) / 1e6, t->cpu->lr, stack.c_str());
    }
}

void wake_all(Cpu& c, uint32_t queue) {
    if (wake_waiters(queue)) reschedule(c);
}

void wait_on(Cpu& c, uint32_t queue) { sleep_on(c, queue); }

void run_on_host_thread(void (*fn)(void*), void* arg) {
    struct Start {
        void (*fn)(void*);
        void* arg;
    };
    auto* s = new Start{fn, arg};
#if defined(_WIN32)
    HANDLE h = CreateThread(nullptr, kHostStack, [](LPVOID p) -> DWORD {
        std::unique_ptr<Start> st(static_cast<Start*>(p));
        set_this_thread_priority(Priority::high);  // the game's threads pace its frames
        st->fn(st->arg);
        return 0;
    }, s, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) rt::fatal(nullptr, "cannot start a host thread");
    CloseHandle(h);
#else
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, kHostStack);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t id;
    if (pthread_create(&id, &a, [](void* p) -> void* {
            std::unique_ptr<Start> st(static_cast<Start*>(p));
            set_this_thread_priority(Priority::high);  // the game's threads pace its frames
            st->fn(st->arg);
            return nullptr;
        }, s) != 0)
        rt::fatal(nullptr, "cannot start a host thread");
    pthread_attr_destroy(&a);
#endif
}

void preempt(Cpu& c) {
    if (!g_current || g_scheduler_off > 0) return;
    Thread* best = best_ready();
    if (best && best->priority < g_current->priority) {
        make_ready(g_current);
        switch_to(best);
    }
    (void)c;
}

void finish_thread(Cpu& c, uint32_t value) {
    Thread* me = g_current;
    rt::poke32(me->guest + kVal, value);
    // Wake without switching: a thread switched away from here would be put
    // back as ready, and a joiner would find it not yet finished.
    for (uint32_t m : std::vector(me->mutexes)) {  // the OS unlocks what an exiting thread holds
        release_mutex(me, m);
        wake_waiters(m);
    }
    me->state = kMoribund;
    if (me->detached) active_remove(me->guest);
    store(me);
    wake_waiters(me->guest + kQueueJoin);
    block(c, true);  // the guest CPU belongs to another thread now; this host thread ends
}

}  // namespace mkdd
