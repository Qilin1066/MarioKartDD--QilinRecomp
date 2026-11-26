// The guest's clock (os.hpp's time_base), in nanoseconds from the host's:
// the host's steady clock less the time the game didn't run.
//
// Two kinds of time are left out. The settings menu's pause: the clock stands
// still while the game is parked (freeze, thaw), so alarms, the retraces and
// the sound go on from where they were. And a stretch no thread of ours saw:
// the PC asleep (Windows' steady clock counts on through sleep), the process
// stopped in a debugger or starved by the system. Every running part of the
// game reads the clock many times a second (the idle loop at least every
// millisecond, the poll clock every few), so when one read follows the last
// by more than kGap, nothing ran in between: the clock carries on from the
// last read instead, as if the host had paused it. Without that the game's
// own clock (OSGetTime) leapt by hours, and code timing itself by it saw one
// enormous frame. Shorter stalls (a long disc read, a hitch) stay in the
// clock as they would on the console; the timers resynchronise after them.
//
// read() is called from every thread and must stay cheap: two atomic loads
// and a compare, a store a millisecond at most, the lock only for a gap.
// It never goes backwards: a value returned without raising `seen_` is under
// seen_ + kNote, and a gap restarts the clock at seen_ + 2 kNote.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace mkdd {

class GuestClock {
public:
    static constexpr int64_t kNote = 1'000'000;      // 1 ms: how stale seen_ may be
    static constexpr int64_t kGap = 2'000'000'000;   // 2 s with no read: the host stopped

    // Called (outside any lock of ours) with each gap left out, in ns.
    void (*on_gap)(int64_t gap_ns) = nullptr;

    // `now()` gives the host's clock. It is read after the offset: a reading
    // taken before a gap was closed and paired with the offset after it would
    // be the gap's length in the past.
    template <class Now>
    int64_t read(Now now) {
        if (const int64_t f = frozen_.load(std::memory_order_acquire); f >= 0) return f;
        const int64_t offset = offset_.load(std::memory_order_acquire);
        const int64_t host_ns = now();
        const int64_t g = host_ns - offset;
        int64_t seen = seen_.load(std::memory_order_relaxed);
        if (g - seen < kNote) return g;
        if (g - seen >= kGap) return close_gap(host_ns);
        while (g > seen && !seen_.compare_exchange_weak(seen, g, std::memory_order_relaxed)) {}
        return g;
    }
    int64_t read_at(int64_t host_ns) { return read([host_ns] { return host_ns; }); }  // (tests)

    // The menu's pause: the clock stands at its reading until thaw. Only the
    // caller that froze it thaws it (freeze returns whether it did).
    bool freeze(int64_t host_ns) {
        const std::lock_guard l(lock_);
        if (frozen_.load(std::memory_order_relaxed) >= 0) return false;
        frozen_.store(read_locked(host_ns), std::memory_order_release);
        return true;
    }
    void thaw(int64_t host_ns) {
        const std::lock_guard l(lock_);
        const int64_t f = frozen_.load(std::memory_order_relaxed);
        if (f < 0) return;
        offset_.store(host_ns - f, std::memory_order_release);  // on from where it stood
        raise_seen(f);
        frozen_.store(-1, std::memory_order_release);           // (after the offset: a reader that sees -1 sees it)
    }

    // A guest reading as the host's clock (for waits on the host's side).
    int64_t host_at(int64_t guest_ns) const { return guest_ns + offset_.load(std::memory_order_acquire); }
    int64_t left_out_ns() const { return left_out_.load(std::memory_order_relaxed); }

private:
    int64_t close_gap(int64_t host_ns) {
        int64_t gap = 0, g;
        {
            const std::lock_guard l(lock_);
            if (const int64_t f = frozen_.load(std::memory_order_relaxed); f >= 0) return f;
            g = host_ns - offset_.load(std::memory_order_relaxed);
            const int64_t seen = seen_.load(std::memory_order_relaxed);
            if (g - seen >= kGap) {  // (another thread may have closed it meanwhile: then not again)
                const int64_t restart = seen + 2 * kNote;
                gap = g - restart;
                offset_.store(host_ns - restart, std::memory_order_release);
                left_out_.fetch_add(gap, std::memory_order_relaxed);
                g = restart;
            } else {
                // Another thread closed it after this one read the host's clock: a reading
                // from before then, against the new offset, would be a little in the past.
                g = std::max(g, seen);
            }
            raise_seen(g);
        }
        if (gap && on_gap) on_gap(gap);
        return g;
    }
    int64_t read_locked(int64_t host_ns) {
        int64_t g = host_ns - offset_.load(std::memory_order_relaxed);
        const int64_t seen = seen_.load(std::memory_order_relaxed);
        if (g - seen >= kGap) {  // (a freeze after a gap: closed here, unannounced)
            const int64_t restart = seen + 2 * kNote;
            offset_.store(host_ns - restart, std::memory_order_release);
            left_out_.fetch_add(g - restart, std::memory_order_relaxed);
            g = restart;
        }
        raise_seen(g);
        return g;
    }
    void raise_seen(int64_t g) {
        int64_t seen = seen_.load(std::memory_order_relaxed);
        while (g > seen && !seen_.compare_exchange_weak(seen, g, std::memory_order_relaxed)) {}
    }

    std::atomic<int64_t> offset_{0};    // the host's reading less the guest's
    std::atomic<int64_t> frozen_{-1};   // the guest's reading while paused, else -1
    std::atomic<int64_t> seen_{0};      // the newest reading handed out, to within kNote
    std::atomic<int64_t> left_out_{0};  // the gaps left out so far
    std::mutex lock_;                   // freeze, thaw and closing a gap
};

}  // namespace mkdd
