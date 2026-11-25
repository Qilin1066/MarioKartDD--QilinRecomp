// Analogue inputs without SDL: shared by the host and regression tests.
#pragma once
#include <algorithm>
#include <cmath>

namespace qilin::host {
// Separate press and release thresholds prevent a noisy axis repeatedly
// pressing a button at the threshold. Percentages of full travel.
inline bool axis_pressed(int value, unsigned threshold, bool held) {
    const int press = static_cast<int>(std::clamp(threshold, 5u, 95u)) * 32767 / 100;
    const int release = std::max(0, press - 32767 * 5 / 100);
    return value > (held ? release : press);
}
// A stick with its dead zone removed: inside `deadzone` percent of its travel
// from the centre it reads as the centre, and from there to the edge it runs
// from 0 to 1 again, in the same direction (a round dead zone, not one per
// axis: a diagonal isn't pulled towards an axis). Past the edge: the edge.
inline void stick_deadzone(float& x, float& y, unsigned deadzone) {
    const float zone = float(std::min(deadzone, 95u)) / 100.0f;
    const float r = std::sqrt(x * x + y * y);
    if (r <= zone || r <= 0.0f) {
        x = y = 0.0f;
        return;
    }
    const float k = std::min((r - zone) / (1.0f - zone), 1.0f) / r;
    x *= k, y *= k;
}
}  // namespace qilin::host
