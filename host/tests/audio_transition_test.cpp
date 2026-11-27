#include "qilin/host/audio.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
}
int main() {
    using namespace qilin::host;
    for (unsigned rate : {32000u, 48000u}) {
        AudioTransitions t;
        t.set_rate(rate);
        const size_t ramp = rate / 500;
        std::vector<int16_t> data(2 * (ramp + 1), 12000);
        t.process(data.data(), ramp + 1, ramp + 1);
        check(data[0] == 0 && data[2 * ramp] == 12000, "playback starts with a bounded fade from silence");
        std::fill(data.begin(), data.end(), -12000);
        t.process(data.data(), ramp + 1, ramp + 1, true);
        check(data[0] == 12000 && data[2 * ramp] == -12000, "a queue trim bridges the old and new samples");
        bool smooth = true;
        for (size_t i = 1; i <= ramp; ++i) smooth &= std::abs(int(data[2 * i]) - data[2 * (i - 1)]) <= 24000 / int(ramp) + 1;
        check(smooth, "the trim does not create a full-amplitude sample jump");
        t.process(data.data(), 0, ramp + 1);
        check(data[0] == -12000 && data[2 * ramp] == 0, "running dry fades the last sample to actual silence");
        std::fill(data.begin(), data.end(), 8000);
        t.process(data.data(), ramp + 1, ramp + 1);
        check(data[0] == 0 && data[2 * ramp] == 8000, "fresh sound resumes from silence without a click");
        for (size_t i = 0; i <= ramp; ++i) data[2 * i] = int16_t(i * 5), data[2 * i + 1] = int16_t(-int(i) * 7);
        const auto expected = data;
        t.process(data.data(), ramp + 1, ramp + 1);
        check(data == expected, "continuous stereo samples pass through bit for bit");
        // Boundaries and ramps can span callbacks shorter than the ramp.
        t.set_rate(rate);
        int16_t pair[2] = {10000, -10000};
        for (size_t i = 0; i <= ramp; ++i) {
            pair[0] = 10000; pair[1] = -10000;
            t.process(pair, 1, 1);
            check(pair[0] >= 0 && pair[1] == -pair[0], "a split ramp preserves stereo polarity");
        }
        check(pair[0] == 10000 && pair[1] == -10000, "a ramp finishes across short callbacks");

        // Run the same pipeline as the host: silence must flush the rear delay,
        // so a gap cannot replay the old rear signal when new sound arrives.
        SurroundDecoder surround;
        surround.set_rate(rate);
        std::vector<int16_t> lr(rate / 20 * 2), six(rate / 20 * 6);
        for (size_t i = 0; i < lr.size() / 2; ++i) lr[2 * i] = 10000, lr[2 * i + 1] = -10000;
        t.process(lr.data(), lr.size() / 2, lr.size() / 2);
        surround.decode(lr.data(), lr.size() / 2, six.data());
        t.process(lr.data(), 0, lr.size() / 2);
        surround.decode(lr.data(), lr.size() / 2, six.data());
        check(six[six.size() - 2] == 0 && six[six.size() - 1] == 0, "a silent callback drains old surround history");
    }
    std::printf("audio_transition_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
