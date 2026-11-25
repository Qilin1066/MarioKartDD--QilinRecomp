// FrameTimes (averages, lows, the rolling window), the sound queue's steering,
// the surround decoder, and the retrace rate chosen for a display.
#include "qilin/host/frame_times.hpp"
#include "qilin/host/audio.hpp"
#include "qilin/host/pacing.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using qilin::host::FrameTimes;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }

int main() {
    FrameTimes t;
    check(t.summary().frames == 0 && t.summary().fps == 0, "empty");

    // 990 frames at 16 ms and 10 at 50 ms: the 1% low is exactly the 50 ms frames.
    for (int i = 0; i < 990; ++i) t.add(16.0);
    for (int i = 0; i < 10; ++i) t.add(50.0);
    auto s = t.summary();
    check(s.frames == 1000, "count");
    check(near(s.average_ms, (990 * 16.0 + 500.0) / 1000.0), "average");
    check(near(s.low1_fps, 20.0), "1% low");
    check(near(s.low01_fps, 20.0), "0.1% low");
    check(near(s.worst_ms, 50.0), "worst");

    // The window rolls: after 1200 more even frames the slow ones are gone.
    for (size_t i = 0; i < FrameTimes::kWindow; ++i) t.add(10.0);
    s = t.summary();
    check(s.frames == FrameTimes::kWindow, "window size");
    check(near(s.fps, 100.0) && near(s.low01_fps, 100.0) && near(s.worst_ms, 10.0), "rolled out");

    // One bad frame in a few: the 0.1% low takes at least one frame.
    t.clear();
    for (int i = 0; i < 9; ++i) t.add(10.0);
    t.add(40.0);
    s = t.summary();
    check(near(s.low01_fps, 25.0) && near(s.low1_fps, 25.0), "small sample lows");

    const auto r = t.recent(3);
    check(r.size() == 3 && r[0] == 10.0f && r[2] == 40.0f, "recent, oldest first");
    check(t.recent(100).size() == 10, "recent clamps");

    // The sound queue's steering: on target plays at 1x; over plays faster,
    // under slower; never beyond 0.5%.
    using qilin::host::audio_rate_ratio;
    check(near(audio_rate_ratio(40, 40), 1.0), "sound on target");
    check(audio_rate_ratio(80, 40) > 1.0 && near(audio_rate_ratio(80, 40), 1.004), "sound over target");
    check(audio_rate_ratio(0, 40) < 1.0 && near(audio_rate_ratio(0, 40), 0.996), "sound under target");
    check(near(audio_rate_ratio(1000, 40), 1.005) && near(audio_rate_ratio(-1000, 40), 0.995), "sound speed bounded");

    // The sound queue itself (frames of left and right; 40 aimed for, 80 the cap).
    {
        using qilin::host::AudioQueue;
        AudioQueue q(40, 80);
        std::vector<int16_t> in(400), out(400);
        for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<int16_t>(i);
        bool dry = false;
        // Nothing plays until the target's worth has gathered.
        check(q.push(in.data(), 30) == 0 && q.pull(out.data(), 10, dry) == 0 && !dry && !q.playing(), "sound waits for its cushion");
        check(q.push(in.data() + 60, 10) == 0 && q.pull(out.data(), 10, dry) == 10 && !dry && q.playing(), "sound starts at the target");
        check(out[0] == 0 && out[1] == 1 && out[19] == 19 && q.size() == 30, "sound comes out oldest first");
        // Steady: 5 in, 5 out, nothing lost and nothing dry.
        size_t dropped = 0, dries = 0;
        for (int i = 0; i < 1000; ++i) {
            dropped += q.push(in.data(), 5);
            q.pull(out.data(), 5, dry);
            dries += dry;
        }
        check(dropped == 0 && dries == 0 && q.size() == 30, "a steady sound queue keeps its length");
        // The device stops for a while (a dropout): the queue passes the cap once and comes back
        // to the target, instead of staying that much longer.
        dropped = 0;
        for (int i = 0; i < 20; ++i) dropped += q.push(in.data(), 5);
        check(dropped == 90 && q.size() == 40, "sound over the cap is dropped to the target");
        // The game stops for a while: the queue runs dry once, plays nothing until the cushion is
        // back, and a burst that catches up is cut to the target.
        size_t got = 0;
        dries = 0;
        for (int i = 0; i < 20; ++i) got += q.pull(out.data(), 5, dry), dries += dry;
        check(dries == 1 && !q.playing() && q.size() == 0, "the sound queue runs dry once");
        check(q.push(in.data(), 20) == 0 && q.pull(out.data(), 5, dry) == 0 && !dry, "after a gap, the cushion first");
        dropped = 0;
        for (int i = 0; i < 20; ++i) dropped += q.push(in.data(), 5);
        check(q.size() <= 80 && dropped > 0 && q.pull(out.data(), 5, dry) == 5, "a catching-up burst is cut to the target");
        (void)got;
    }

    // The retrace rate for a display (pacing.hpp): the console's own 59.94 Hz unless the display's
    // refresh is a whole multiple of it to within half a percent, when the display's is followed.
    {
        using qilin::host::retrace_rate;
        const auto free_running = [](const qilin::host::RetraceRate& x) { return !x.locked && x.num == 60000 && x.den == 1001; };
        check(near(retrace_rate(0, 0, true, false).hz(), 59.94005994), "no display known: 59.94 Hz by the clock");
        check(free_running(retrace_rate(0, 0, true, false)), "and not locked");
        auto rr = retrace_rate(60000, 1001, true, false);
        check(rr.locked && rr.every == 1 && rr.num == 60000 && rr.den == 1001, "a 59.94 Hz display: one frame a refresh");
        rr = retrace_rate(60, 1, true, false);
        check(rr.locked && rr.every == 1 && near(rr.hz(), 60.0), "a 60 Hz display: followed, a thousandth fast");
        rr = retrace_rate(59951, 1000, true, false);
        check(rr.locked && rr.every == 1 && near(rr.hz(), 59.951), "a display a little off its nominal rate is followed exactly");
        rr = retrace_rate(120000, 1001, true, false);
        check(rr.locked && rr.every == 2 && near(rr.hz(), 59.94005994), "119.88 Hz: every second refresh");
        rr = retrace_rate(120, 1, true, false);
        check(rr.locked && rr.every == 2 && near(rr.hz(), 60.0), "120 Hz: every second refresh");
        rr = retrace_rate(180, 1, true, false);
        check(rr.locked && rr.every == 3, "180 Hz: every third");
        rr = retrace_rate(240, 1, true, false);
        check(rr.locked && rr.every == 4 && near(rr.hz(), 60.0), "240 Hz: every fourth");
        rr = retrace_rate(239760, 1000, true, false);
        check(rr.locked && rr.every == 4 && near(rr.hz(), 59.94), "239.76 Hz: every fourth");
        check(retrace_rate(360, 1, true, false).every == 6 && retrace_rate(480, 1, true, false).every == 8, "360 and 480 Hz");
        // Not multiples: the game keeps its own rate (frames land unevenly; fullscreen looks for a mode that is one).
        for (uint32_t hz : {50u, 72u, 75u, 85u, 90u, 100u, 144u, 165u, 170u, 200u, 250u, 500u})
            check(free_running(retrace_rate(hz, 1, true, false)), "a refresh that isn't a multiple of 59.94 Hz: the console's own rate");
        check(free_running(retrace_rate(59500, 1000, true, false)) && free_running(retrace_rate(60400, 1000, true, false)),
              "more than half a percent off isn't followed (the game's speed would show)");
        check(retrace_rate(59700, 1000, true, false).locked && retrace_rate(60250, 1000, true, false).locked, "within it is");
        check(free_running(retrace_rate(60, 1, false, false)) && free_running(retrace_rate(120, 1, false, false)), "without vsync: the console's own rate");
        check(free_running(retrace_rate(144, 1, true, true)) && free_running(retrace_rate(60, 1, true, true)),
              "a variable-refresh display follows the game: the console's own rate");
        check(free_running(retrace_rate(60, 0, true, false)), "a refresh with no denominator is no refresh");
        using qilin::host::even_refresh;
        check(even_refresh(59.94) && even_refresh(60.0) && even_refresh(119.88) && even_refresh(120.0) && even_refresh(240.0) &&
                  !even_refresh(144.0) && !even_refresh(165.0) && !even_refresh(75.0) && !even_refresh(50.0) && !even_refresh(30.0),
              "the modes fullscreen counts as even");
    }

    // The surround decoder (audio.hpp): what is alike in both channels is the centre's, what is
    // opposite the rear's (late and band-limited), and left and right pass as they are.
    {
        qilin::host::SurroundDecoder dec;
        dec.set_rate(32000);
        const size_t n = 3200;  // a tenth of a second
        std::vector<int16_t> in(n * 2), out(n * 6);
        const auto tone = [](size_t i) { return static_cast<int16_t>(std::lround(8000.0 * std::sin(2.0 * 3.14159265358979 * 400.0 * double(i) / 32000.0))); };
        const auto peak = [&](size_t channel, size_t from) {
            int most = 0;
            for (size_t i = from; i < n; ++i) most = std::max(most, std::abs(int(out[i * 6 + channel])));
            return most;
        };
        for (size_t i = 0; i < n; ++i) in[2 * i] = in[2 * i + 1] = tone(i);  // the same in both
        dec.decode(in.data(), n, out.data());
        check(out[6 * 100] == in[2 * 100] && out[6 * 100 + 1] == in[2 * 100 + 1], "surround: left and right pass unchanged");
        check(std::abs(peak(2, 0) - 11314) < 40 && peak(4, 0) == 0 && peak(5, 0) == 0 && peak(3, 0) == 0,
              "surround: a sound alike in both is the centre's alone, 3 dB up on either");
        dec.set_rate(32000);
        for (size_t i = 0; i < n; ++i) in[2 * i] = tone(i), in[2 * i + 1] = static_cast<int16_t>(-tone(i));  // opposite
        dec.decode(in.data(), n, out.data());
        check(peak(2, 0) == 0 && peak(4, 640) > 7000 && peak(4, 640) < 8100 && peak(4, 640) == peak(5, 640),
              "surround: a sound opposite in the two is the rear's, in both rear speakers");
        bool silent = true;
        for (size_t i = 0; i < 320; ++i) silent = silent && out[i * 6 + 4] == 0;
        check(silent && out[(320 + 40) * 6 + 4] != 0, "surround: the rear is 10 ms late");
        for (size_t i = 0; i < n; ++i) in[2 * i] = tone(i), in[2 * i + 1] = 0;  // left only
        dec.set_rate(48000);
        dec.decode(in.data(), n, out.data());
        check(peak(0, 0) == 8000 && peak(1, 0) == 0 && peak(2, 0) > 5000 && peak(4, 960) > 3000, "surround: one side alone leaks to centre and rear, as the matrix does");
    }

    if (failures) return EXIT_FAILURE;
    std::puts("frame_times_test: ok");
    return EXIT_SUCCESS;
}
