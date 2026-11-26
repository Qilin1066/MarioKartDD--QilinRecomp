// What the devices' tests stand on instead of the OS layer (os.cpp,
// threads.cpp): a clock the test moves, the queue of interrupts, the timer
// sources, and guest callbacks recorded rather than run. A thread that
// sleeps (wait_on) is the test itself, so the clock runs on to each timed
// event until something wakes it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>
#include <vector>

#include "os.hpp"
#include "threads.hpp"

namespace harness {
namespace rt = qilin::rt;

struct Callback {
    uint32_t fn, a0, a1;
    uint64_t at;
    bool operator==(const Callback&) const = default;
};
inline uint64_t now = 1000;
inline std::vector<std::function<void(rt::Cpu&)>> pending;
inline std::vector<std::pair<std::function<void()>, std::function<uint64_t()>>> sources;
inline std::vector<Callback> callbacks;
inline bool in_interrupt = false, woken = false;
inline int failures = 0;

inline void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}
// Deliver what is due now.
inline void deliver(rt::Cpu& c) {
    for (bool again = true; again;) {
        for (auto& s : sources) s.first();
        again = !pending.empty();
        auto work = std::move(pending);
        pending.clear();
        in_interrupt = true;
        for (auto& w : work) w(c);
        in_interrupt = false;
    }
}
inline uint64_t next_deadline() {
    uint64_t t = UINT64_MAX;
    for (auto& s : sources) t = std::min(t, s.second());
    return t;
}
// Let `ticks` pass, delivering each event at its own time.
inline void advance(rt::Cpu& c, uint64_t ticks) {
    const uint64_t end = now + ticks;
    deliver(c);
    for (uint64_t t; (t = next_deadline()) <= end;) {
        now = std::max(now, t);
        deliver(c);
    }
    now = end;
}
inline uint64_t ms(uint64_t n) { return n * mkdd::kTimerHz / 1000; }
}  // namespace harness

namespace mkdd {
void post_interrupt(std::function<void(qilin::rt::Cpu&)> work) { harness::pending.push_back(std::move(work)); }
void add_timer_source(std::function<void()> poll, std::function<uint64_t()> deadline) {
    harness::sources.emplace_back(std::move(poll), std::move(deadline));
}
uint64_t time_base() { return harness::now; }
void note_deadline(uint64_t) {}  // (no poll clock: the test moves time itself)
bool in_interrupt() { return harness::in_interrupt; }
void guest_callback(qilin::rt::Cpu&, uint32_t fn, uint32_t a0, uint32_t a1) {
    harness::callbacks.push_back({fn, a0, a1, harness::now});
}
void wait_on(qilin::rt::Cpu& c, uint32_t) {
    harness::woken = false;
    while (!harness::woken) {
        const uint64_t t = harness::next_deadline();
        if (t == UINT64_MAX && harness::pending.empty()) {
            std::fprintf(stderr, "FAIL a thread sleeps with nothing left to wake it\n");
            std::exit(1);
        }
        if (t != UINT64_MAX) harness::now = std::max(harness::now, t);
        harness::deliver(c);
    }
}
void wake_all(qilin::rt::Cpu&, uint32_t) { harness::woken = true; }
uint32_t frame_count() { return 0; }  // (video.hpp: for the devices' development log)
}  // namespace mkdd
