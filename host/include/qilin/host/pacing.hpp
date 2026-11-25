// The retrace rate for a display: pure arithmetic, apart from the window so
// it can be tested (host/tests/frame_times_test.cpp).
//
// The console draws 59.94 fields a second (NTSC: 60000/1001). A display
// refreshes at its own rate, by its own crystal: left alone, the two drift,
// and every few seconds a frame is shown twice or not at all. So where the
// display's refresh is a whole multiple of the console's, near enough that
// nobody could tell the speed apart (0.3 Hz in 60: half a percent), the
// retraces are run from the display instead: one frame per refresh, or every
// second, third ... one. Otherwise (75, 144, 165 Hz in a window; no vsync; a
// variable-refresh display, which follows the game by itself) they run free
// at the console's own rate.
#pragma once

#include <cmath>
#include <cstdint>

namespace qilin::host {

inline constexpr uint32_t kNtscNum = 60000, kNtscDen = 1001;  // 59.94 Hz

struct RetraceRate {
    uint32_t num = kNtscNum, den = kNtscDen;  // retraces a second
    bool locked = false;                      // run from the display's refresh
    uint32_t every = 0;                       // locked: one frame per this many refreshes
    double hz() const { return double(num) / double(den); }
};

// `refresh_num / refresh_den`: the display's refresh (0: unknown). Locks to
// refreshes of 59.64 to 60.3 Hz and their multiples up to eight (480 Hz).
inline RetraceRate retrace_rate(uint32_t refresh_num, uint32_t refresh_den, bool vsync, bool variable_refresh) {
    RetraceRate r;
    if (!vsync || variable_refresh || refresh_num == 0 || refresh_den == 0) return r;
    const double hz = double(refresh_num) / double(refresh_den);
    for (uint32_t k = 1; k <= 8; ++k) {
        const double each = hz / k;
        if (each >= 59.94 - 0.3 && each <= 60.0 + 0.3) {
            r.num = refresh_num, r.den = refresh_den * k, r.locked = true, r.every = k;
            return r;
        }
    }
    return r;
}

// Whether a mode's refresh shows the console's frames evenly (fullscreen
// picks such a mode where the desktop's isn't one).
inline bool even_refresh(double hz) {
    for (int k = 1; k <= 8; ++k)
        if (hz / k >= 59.94 - 0.3 && hz / k <= 60.0 + 0.3) return true;
    return false;
}

}  // namespace qilin::host
