// The guest's clock (guest_clock.hpp): pauses and host stops are left out, and
// it never runs backwards, from one thread or several.
#include "guest_clock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}
int64_t g_gaps = 0, g_gap_ns = 0;
}  // namespace

int main() {
    using mkdd::GuestClock;
    constexpr int64_t ms = 1'000'000, s = 1'000'000'000;
    {
        GuestClock c;
        c.on_gap = [](int64_t ns) { ++g_gaps, g_gap_ns += ns; };
        int64_t host = 0, last = 0;
        bool steady = true;
        for (int i = 0; i < 2000; ++i) {  // read every millisecond: the clock is the host's
            host += ms;
            const int64_t g = c.read_at(host);
            steady = steady && g == host && g >= last;
            last = g;
        }
        check(steady, "reads every millisecond follow the host exactly");
        host += 1500 * ms;  // a stall shorter than the gap: kept, as on the console
        check(c.read_at(host) == host, "a 1.5 s stall stays in the clock");
        const int64_t before = c.read_at(host);
        host += 3 * 3600 * s;  // the PC asleep for three hours
        const int64_t after = c.read_at(host);
        check(after > before && after - before <= 2 * GuestClock::kNote, "three hours asleep: the clock goes on from where it was");
        check(g_gaps == 1 && g_gap_ns > 3 * 3600 * s - s, "the gap is announced once, with its length");
        check(c.read_at(host + 10 * ms) == after + 10 * ms, "and then follows the host again");
        host += 10 * ms;
        // The menu's pause: the clock stands still, then goes on from there.
        const int64_t paused_at = c.read_at(host);
        check(c.freeze(host), "freeze");
        check(!c.freeze(host + ms), "a second freeze doesn't take over");
        check(c.read_at(host + 30 * s) == paused_at, "frozen: the clock stands still for 30 s of host time");
        host += 60 * s;
        c.thaw(host);
        check(c.read_at(host) == paused_at && c.read_at(host + 5 * ms) == paused_at + 5 * ms, "thawed: on from where it stood");
        check(g_gaps == 1, "a pause isn't a gap");
        check(c.host_at(paused_at + 5 * ms) == host + 5 * ms, "a guest reading maps back to the host's clock");
    }
    {
        // A freeze long after the last read (the menu opened just after a stop) closes the gap too.
        GuestClock c;
        c.read_at(10 * ms);
        c.freeze(10 * s);
        check(c.read_at(11 * s) <= 10 * ms + 2 * GuestClock::kNote, "a freeze after a stop leaves the stop out");
        c.thaw(12 * s);
    }
    {
        // Several threads reading one host clock that jumps: each sees a clock that never goes back,
        // and the jump is left out once.
        GuestClock c;
        g_gaps = 0;
        c.on_gap = [](int64_t) { ++g_gaps; };
        std::atomic<int64_t> host{0};
        std::atomic<bool> stop{false}, backwards{false};
        std::atomic<int64_t> reads{0};
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t)
            readers.emplace_back([&] {
                int64_t last = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    const int64_t g = c.read([&] { return host.load(std::memory_order_relaxed); });
                    if (g < last) backwards.store(true);
                    last = g;
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        // Each step waits for a few reads, so the readers run throughout (and after the jump).
        const auto steps = [&](int n, int64_t step) {
            for (int i = 0; i < n; ++i) {
                host.fetch_add(step);
                for (const int64_t r = reads.load(); reads.load() < r + 8;) std::this_thread::yield();
            }
        };
        steps(5000, 10'000);  // 10 us steps
        steps(1, 100 * s);    // once 100 s
        steps(5000, 10'000);
        stop.store(true);
        for (auto& r : readers) r.join();
        check(!backwards.load(), "four threads: no reading goes backwards");
        std::printf("(gaps %lld, %.3f s left out)\n", static_cast<long long>(g_gaps), double(c.left_out_ns()) / 1e9);
        check(g_gaps == 1 && c.left_out_ns() > 99 * s, "the jump is left out once");
    }
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
