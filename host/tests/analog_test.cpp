#include "qilin/host/analog.hpp"
#include <cstdio>
#include <cmath>
#include <utility>
int main() {
    using namespace qilin::host;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++failures;
    };
    check(!axis_pressed(16000, 50, false), "resting below threshold");
    check(axis_pressed(17000, 50, false), "crossing press threshold");
    check(axis_pressed(15500, 50, true), "jitter does not release a held input");
    check(!axis_pressed(14000, 50, true), "releasing below hysteresis");
    check(!axis_pressed(15500, 50, false), "jitter cannot press it again");
    check(axis_pressed(32768, 95, false), "negative stick endpoint (negated INT16_MIN)");
    check(!axis_pressed(0, 5, true), "centre always releases");
    const auto stick = [](float x, float y, unsigned zone) {
        stick_deadzone(x, y, zone);
        return std::pair<float, float>(x, y);
    };
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
    check(stick(0.2f, 0.1f, 25) == std::pair<float, float>(0.0f, 0.0f), "stick drift removed");
    check(near(stick(1.0f, 0.0f, 25).first, 1.0f) && near(stick(0.0f, -1.0f, 25).second, -1.0f), "both endpoints kept");
    check(near(stick(0.625f, 0.0f, 25).first, 0.5f), "half way from the dead zone's edge to the stick's is half");
    {
        const auto d = stick(0.5f, 0.5f, 25);
        check(near(d.first, d.second) && d.first > 0.0f, "a diagonal keeps its direction (a round dead zone)");
        const auto a = stick(0.3f, -0.6f, 15), b = stick(-0.3f, 0.6f, 15);
        check(near(a.first, -b.first) && near(a.second, -b.second), "symmetry");
        const auto over = stick(1.0f, 1.0f, 15);
        check(near(std::sqrt(over.first * over.first + over.second * over.second), 1.0f), "past the edge (a square gate's corner) is the edge");
    }
    check(near(stick(0.4f, 0.0f, 0).first, 0.4f), "no dead zone: the stick as it is");
    check(std::isfinite(stick(1.0f, 0.0f, 100).first) && std::isfinite(stick(0.0f, 0.0f, 0).first), "an invalid dead zone, or the centre, cannot divide by zero");
    return failures ? 1 : 0;
}
