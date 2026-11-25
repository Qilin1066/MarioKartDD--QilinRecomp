// mkdd: the recompiled game.
//
//   mkdd [--headless | --offscreen] <folder with the extracted disc: sys/main.dol, sys/boot.bin, files/>
//
// The guest runs on threads of its own (threads.cpp); this thread owns the
// window and the GPU: it handles the window's events, draws the frames the
// guest records (renderer.cpp) and shows the frame buffer VI names.
// --headless runs without a window, GPU or sound; --offscreen draws without a
// window (with MKDD_DUMP=<folder>, every 60th frame shown is saved there as
// PPM; MKDD_DUMP_EVERY=n changes the 60).
// mkdd.ini in the game's data folder holds the settings (settings.hpp),
// which the settings menu (F1 or Esc, menu.hpp) changes; MKDD_SCALE
// overrides the render resolution.
// MKDD_INPUT plays the first controller port on a schedule, for runs without
// a player (MKDD_INPUT2 to MKDD_INPUT4 the others; a port with a script is
// connected from the start): "Start@12,A@14.5,Stick:-100:0@20+3" holds each
// for a fifth of a second, or for the seconds after a +, from that many
// seconds of game time (the game's frames over 60, so a slow GPU doesn't
// shift them); input_map.hpp has the whole of it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <thread>
#include <string>
#include <vector>

#include "boot.hpp"
#include "audio.hpp"
#include "hw.hpp"
#include "gx.hpp"
#include "gpu.hpp"
#include "enhance.hpp"
#include "input.hpp"
#include "input_map.hpp"
#include "menu.hpp"
#include "os.hpp"
#include "priority.hpp"
#include "profile.hpp"
#include "settings.hpp"
#include "save_history.hpp"
#include "speaker.hpp"
#include "build.hpp"
#include "qilin/host/paths.hpp"
#include "qilin/rt/report.hpp"
#include "renderer.hpp"
#include "recorder.hpp"
#include "qilin/host/audio.hpp"
#include "qilin/host/display.hpp"
#include "qilin/host/pacing.hpp"
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include "threads.hpp"
#include "video.hpp"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

// The lifted code's tables (qilin lift's table.cpp and replaced.cpp), declared
// here rather than through its functions.hpp, so this file builds without the
// lifted code (the Windows download's prebuilt part, tools/player-kit).
namespace qilin::gen {
extern const ::qilin::rt::FunctionEntry kFunctions[];
extern const unsigned kFunctionCount;
extern const ::qilin::rt::ReplacedEntry kReplaced[];
extern const unsigned kReplacedCount;
}  // namespace qilin::gen

namespace {
bool g_display_locked = false;  // the retraces run at the display's refresh (or a whole fraction of it)

// Vblank steering (Beat the Beat's, Windows): with the retraces locked to the
// display, nudge their speed (within 0.2%) so each frame is presented a few
// ms before a vblank, every frame alike: shown as soon after it's drawn as the
// display allows, without the slow drift between the two clocks bringing
// spells where a frame waits most of a refresh. A PI loop on the time from
// the present to the next vblank (the slack), against a target of 1 ms plus
// four standard deviations of its spread, at least 3 ms.
struct VblankSteer {
    double mean = 0, var = 0, integral = 0, delta = 0;
    uint64_t last = 0, frames = 0, late = 0, slack_count = 0;
    double slack_sum = 0, target = 0;
} g_vbl;
void steer_to_vblank() {
    uint64_t vblank = 0, period = 0;
    if (!g_display_locked || !mkdd::display_vblank(vblank, period)) return;
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const double dt = g_vbl.last ? double(now - g_vbl.last) : 0;
    g_vbl.last = now;
    if (dt <= 0 || dt > 4.0 * 1e9 / 59.0) {  // a long gap (loading, the menu): start the averages again
        g_vbl.frames = 0;
        return;
    }
    const double p = double(period);
    double since = std::fmod(double(int64_t(now - vblank)), p);
    if (since < 0) since += p;
    const double slack = p - since;  // to the next vblank
    g_vbl.target = std::clamp(1e6 + 4.0 * std::sqrt(g_vbl.var), 3.0e6, p / 2);
    double e = slack - g_vbl.target;
    if (e > p / 2) e -= p;
    if (e < -p / 2) e += p;
    if (g_vbl.frames >= 16 && std::abs(g_vbl.mean) < 0.5e6 && slack > p - 0.5e6) ++g_vbl.late;  // just missed one
    ++g_vbl.frames;
    constexpr double a = 1.0 / 16;  // about a quarter of a second
    if (g_vbl.frames == 1) g_vbl.mean = e, g_vbl.var = 0;
    const double d = e - g_vbl.mean;
    g_vbl.mean += a * d;
    g_vbl.var = (1 - a) * (g_vbl.var + a * d * d);
    if (g_vbl.frames < 16) return;
    g_vbl.slack_sum += slack, ++g_vbl.slack_count;
    // Presenting early (slack above the target) slows the game a little. Kp 1/s, Ki 0.25/s^2.
    const double es = g_vbl.mean / 1e9, dts = dt / 1e9;
    g_vbl.integral = std::clamp(g_vbl.integral + 0.25 * es * dts, -0.001, 0.001);
    g_vbl.delta = std::clamp(-(1.0 * es + g_vbl.integral), -0.002, 0.002);
    if (g_vbl.frames % 4 == 0) mkdd::set_retrace_speed(1.0 + g_vbl.delta);
}

// The retrace rate for the display the window is on (video.hpp's
// set_retrace_rate), as host/pacing.hpp chooses it: the display's refresh, or
// a whole fraction of it, when that is the console's 59.94 Hz to within half
// a percent; otherwise, without vsync and on a variable-refresh display
// (which follows the game on its own), the console's own 59.94 Hz by the
// clock. Logged when it changes.
void match_retrace_to_display(qilin::host::Display& display) {
    namespace rt = qilin::rt;
    static uint64_t chosen = 0;  // num << 32 | den
    uint32_t n = 0, d = 0;
    auto* window = static_cast<SDL_Window*>(display.sdl_window());
    if (const SDL_DisplayMode* m = window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)) : nullptr) {
        n = static_cast<uint32_t>(m->refresh_rate_numerator), d = static_cast<uint32_t>(m->refresh_rate_denominator);
        if (n == 0 || d == 0) n = static_cast<uint32_t>(std::lround(m->refresh_rate * 1000.0)), d = 1000;
    }
    const double display_hz = n && d ? double(n) / double(d) : 0;
    const qilin::host::RetraceRate r = qilin::host::retrace_rate(n, d, display.picture().vsync, display.picture().variable_refresh);
    g_display_locked = r.locked;
    mkdd::g_display_hz.store(display_hz, std::memory_order_relaxed);
    const uint64_t key = uint64_t(r.num) << 32 | r.den;
    if (key == chosen) return;
    chosen = key;
    mkdd::set_retrace_rate(r.num, r.den);
    if (r.locked)
        rt::log("retraces at %.3f Hz, matching the display's %.3f Hz refresh (a frame every %u)", r.hz(), display_hz, r.every);
    else
        rt::log("retraces at %.3f Hz, the console's own%s", r.hz(),
                !display.picture().vsync              ? " (no vsync)"
                : display.picture().variable_refresh ? " (variable refresh)"
                : display_hz > 0                     ? " (the display's refresh isn't near a multiple of it: frames are uneven)"
                                                     : "");
}
}  // namespace

// Passes of the main loop, for the watchdog.
static std::atomic<uint64_t> g_main_passes{0};

// A thread that only watches: says in the log when the game draws no new
// frame for 10 s while not paused (stuck, or a long load), and when the main
// loop (input, drawing, presenting) stops for 5 s, and when each goes again.
// It changes nothing: the log is for working out what happened.
static void start_watchdog() {
    std::thread([] {
        namespace rt = qilin::rt;
        mkdd::set_this_thread_priority(mkdd::Priority::background);
        uint32_t frames = mkdd::frame_count();
        uint64_t passes = g_main_passes.load(std::memory_order_relaxed);
        unsigned game_still = 0, main_still = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint32_t f = mkdd::frame_count();
            if (f != frames || mkdd::game_parked()) {
                if (game_still >= 10) rt::log("watchdog: the game draws again, after %u s without a new frame", game_still);
                frames = f, game_still = 0;
            } else if (++game_still == 10) {
                rt::log("watchdog: the game has drawn no new frame for 10 s (stuck, or a long load)");
            }
            const uint64_t p = g_main_passes.load(std::memory_order_relaxed);
            if (p != passes) {
                if (main_still >= 5) rt::log("watchdog: the main loop runs again, after %u s stopped", main_still);
                passes = p, main_still = 0;
            } else if (++main_still == 5) {
                rt::log("watchdog: the main loop (input, drawing, presenting) has been stopped for 5 s");
            }
        }
    }).detach();
}

// The F3 readout's details (each second) and the timing log (each 5 s).
static void report_timing(qilin::host::Display* display, mkdd::Menu* menu, qilin::host::AudioOut* audio) {
    namespace rt = qilin::rt;
    using clock = std::chrono::steady_clock;
    static auto next = clock::now() + std::chrono::seconds(1);
    static unsigned seconds = 0;
    const auto t = clock::now();
    if (t < next) return;
    next = t + std::chrono::seconds(1);
    // Per frame over the last second: the game's recording, the main thread's
    // drawing, the GPU's own time, and waiting to present.
    struct Totals {
        uint64_t frames, record, rendered, render, gpu_ns, gpu_timed;
    };
    const mkdd::gx::RecorderStats& g = mkdd::gx::recorder_stats();
    const auto rd = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
    const Totals now{rd(g.frames), rd(g.record_ns), rd(g.rendered), rd(g.render_ns), rd(g.gpu_ns), rd(g.gpu_timed)};
    static Totals last = now;
    const auto per = [](uint64_t a, uint64_t b, uint64_t n) { return n ? double(a - b) / 1e6 / double(n) : 0.0; };
    const double record = per(now.record, last.record, now.frames - last.frames);
    const double draw = per(now.render, last.render, now.rendered - last.rendered);
    const double gpu_ms = per(now.gpu_ns, last.gpu_ns, now.gpu_timed - last.gpu_timed);
    last = now;
    double present_wait = 0.0;
    if (display) {
        const auto ps = display->take_present_stats();
        present_wait = ps.presents ? ps.wait_ms / ps.presents : 0.0;
    }
    // Sound: kept over the 5 s for the log.
    static qilin::host::AudioOut::Stats sound{};
    static uint64_t sound_pushes = 0;
    static double sound_sum = 0;
    double sound_now = 0;
    if (audio) {
        const auto a = audio->take_stats();
        sound_now = a.average_ms;
        sound_sum += a.average_ms * a.pushes, sound_pushes += a.pushes;
        sound.max_ms = std::max(sound.max_ms, a.max_ms);
        sound.empty += a.empty, sound.dropped += a.dropped, sound.dropped_ms += a.dropped_ms, sound.ratio = a.ratio;
    }
    if (menu && menu->frame_times_shown()) {
        char a[96], b[96];
        std::snprintf(a, sizeof a, "game %.1f ms  draw %.1f ms  GPU %.1f ms", record, draw, gpu_ms);
        std::snprintf(b, sizeof b, "present wait %.1f ms  sound %.0f ms queued", present_wait, sound_now);
        menu->set_frame_details({a, b});
    }
    if (++seconds % 5) return;
    const auto ft = display ? display->frame_times().summary() : qilin::host::FrameTimes::Summary{};
    if (ft.frames)
        rt::log("  frame times (last %.0f s): %.1f fps (%.2f ms), 1%% low %.1f fps, 0.1%% low %.1f fps, worst %.1f ms; waiting to present %.2f ms a frame",
                ft.average_ms * double(ft.frames) / 1000.0, ft.fps, ft.average_ms, ft.low1_fps, ft.low01_fps, ft.worst_ms, present_wait);
    if (g_vbl.slack_count) {
        rt::log("  vblank steering: presented %.2f ms before the vblank on average (target %.2f), %llu just missed; game clock %+.3f%%",
                g_vbl.slack_sum / double(g_vbl.slack_count) / 1e6, g_vbl.target / 1e6, static_cast<unsigned long long>(g_vbl.late),
                g_vbl.delta * 100.0);
        g_vbl.slack_sum = 0, g_vbl.slack_count = 0, g_vbl.late = 0;
    }
    if (audio && sound_pushes) {
        rt::log("  sound: %.1f ms queued on average (aiming for %.0f), %.1f at most, played at %.4fx; ran dry %u times, dropped %.0f ms in %u times",
                sound_sum / double(sound_pushes), audio->target_ms(), sound.max_ms, sound.ratio, sound.empty, sound.dropped_ms, sound.dropped);
        sound = {}, sound_sum = 0, sound_pushes = 0;
    }
}

// --check-speaker: the remote speaker's decoder (speaker.hpp) against the encoder in the game's
// own code (the SDK's WENC, lifted: (state, 0 first or 1 going on, samples, count, bytes out)).
// The encoder keeps the signal as the remote will decode it, and its step, in its state: after
// every block of every test signal both must be exactly the decoder's, and they are looked for
// there (wherever the SDK keeps them). No game runs: only that one function.
static int check_speaker_decoder() {
    namespace rt = qilin::rt;
    constexpr uint32_t kEncode = 0x802166B4u, kState = 0x80F00000u, kPcm = 0x80F00100u, kOut = 0x80F00200u;
    rt::Cpu c = rt::new_cpu();
    rt::set_thread_cpu(&c);
    unsigned blocks = 0, exact = 0;
    double worst = 0;
    int where_signal = -1, where_step = -1;
    for (int signal = 0; signal < 5; ++signal) {
        mkdd::SpeakerDecoder d;
        std::memset(rt::guest_ptr(kState), 0, 0x100);
        uint32_t noise = 12345;
        for (int block = 0; block < 400; ++block) {
            int16_t pcm[40];
            for (int i = 0; i < 40; ++i) {
                const double t = (block * 40 + i) / 6000.0;
                noise = noise * 1664525u + 1013904223u;
                const double v = signal == 0   ? 20000 * std::sin(6.283185307179586 * 440 * t)
                                 : signal == 1 ? 32767 * std::sin(6.283185307179586 * (100 + 2000 * t) * t)
                                 : signal == 2 ? (int32_t(noise >> 16) - 32768) * 0.9
                                 : signal == 3 ? ((block / 3) % 2 ? 32767.0 : -32768.0)
                                               : 0.0;
                pcm[i] = static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0));
                rt::poke16(kPcm + 2 * uint32_t(i), static_cast<uint16_t>(pcm[i]));
            }
            c.r[1] = mkdd::build::kStackTop - 0x100, c.r[2] = mkdd::build::kSdaBase2, c.r[13] = mkdd::build::kSdaBase;
            c.r[3] = kState, c.r[4] = block == 0 ? 0 : 1, c.r[5] = kPcm, c.r[6] = 40, c.r[7] = kOut;
            rt::call(c, kEncode);
            int16_t out[40];
            d.decode(rt::guest_ptr(kOut), 20, out);
            for (int i = 0; i < 40; ++i) worst = std::max(worst, std::fabs(double(out[i]) - pcm[i]));
            // The decoder's signal and step, somewhere in the encoder's state (32-bit words).
            ++blocks;
            bool has_signal = false, has_step = false;
            for (int at = 0; at < 0x40; at += 4) {
                const auto word = static_cast<int32_t>(rt::peek32(kState + uint32_t(at)));
                if (word == d.signal && (where_signal < 0 || where_signal == at) && !has_signal) {
                    if (d.signal != 0) where_signal = at;
                    has_signal = true;
                }
                if (word == d.step && (where_step < 0 || where_step == at)) where_step = at, has_step = true;
            }
            if (has_signal && has_step) ++exact;
            else if (blocks - exact <= 3)
                std::printf("  signal %d, block %d: the decoder has signal %d and step %d; the encoder's state: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                            signal, block, d.signal, d.step, rt::peek32(kState), rt::peek32(kState + 4), rt::peek32(kState + 8),
                            rt::peek32(kState + 12), rt::peek32(kState + 16), rt::peek32(kState + 20), rt::peek32(kState + 24), rt::peek32(kState + 28));
        }
    }
    std::printf("the speaker's decoder against the game's encoder: %u blocks of 40 samples, %u with the encoder's signal and step exactly the "
                "decoder's (kept at +%d and +%d of its state); the codec's own largest error, on a full-scale square wave: %.0f\n",
                blocks, exact, where_signal, where_step, worst);
    std::fflush(nullptr);
    return blocks == exact ? 0 : 1;
}

int main(int argc, char** argv) {
    namespace rt = qilin::rt;
    mkdd::set_command_line(argc, argv);  // for the game's reset
    mkdd::start_profiler();              // MKDD_PROFILE, for development
#if defined(_WIN32)
    // Windows' timers tick every 15.6 ms unless a program asks for finer: then a
    // sleep to a retrace could end a whole frame late. Ended with the process.
    timeBeginPeriod(1);
#endif
    mkdd::tune_process();  // Windows 11: no throttling when the window is hidden or covered
    // The main loop presents frames: ahead of shader compiles and cache saves.
    mkdd::set_this_thread_priority(mkdd::Priority::high);
    bool headless = false, offscreen = false;
    const char* folder = nullptr;
    // --bench-record <file.dff> [times]: time the recorder on a captured frame (gpu.hpp).
    const char* bench_dff = nullptr;
    unsigned bench_times = 200;
    if (argc >= 3 && std::strcmp(argv[1], "--bench-record") == 0) {
        bench_dff = argv[2];
        if (argc >= 4) bench_times = unsigned(std::max(1, std::atoi(argv[3])));
        folder = argv[2];
        argc = 1;  // nothing else to read
    }
    // --render-dff <file.dff> <out.ppm> [loops]: draw a captured frame with our renderer (below).
    const char* render_dff = nullptr;
    const char* render_out = nullptr;
    unsigned render_loops = 1;
    if (argc >= 4 && std::strcmp(argv[1], "--render-dff") == 0) {
        render_dff = argv[2], render_out = argv[3];
        if (argc >= 5) render_loops = unsigned(std::max(1, std::atoi(argv[4])));
        folder = argv[2];
        argc = 1;
    }
    // --check-speaker <disc>: the Wii Remote speaker's decoder against the game's own encoder (below).
    bool check_speaker = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--check-speaker") == 0) check_speaker = headless = true;
        else if (std::strcmp(argv[i], "--headless") == 0) headless = true;
        else if (std::strcmp(argv[i], "--offscreen") == 0) offscreen = true;
        else if (!folder) folder = argv[i];
        else folder = nullptr, i = argc;  // too many: usage
    }
    if (!folder) {
        std::fprintf(stderr, "usage: mkdd [--headless | --offscreen] <folder with the extracted disc: sys/main.dol, sys/boot.bin, files/>\n"
                             "       mkdd --bench-record <file.dff> [times] | --render-dff <file.dff> <out.ppm> [loops]\n"
                             "       mkdd --check-speaker <folder>\n");
        return 2;
    }
    // The log, beside the settings: mkdd.log for this run, mkdd.old.log
    // the run before. On Windows the console closes with the game, so after
    // a crash the file is what's left to read (or send).
    rt::install_crash_handler();
    if (!bench_dff && !render_dff) {  // (a benchmark or a replay leaves the game's log alone)
        const std::filesystem::path dir = qilin::host::user_folder(), log = dir / "mkdd.log";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::rename(log, dir / "mkdd.old.log", ec);
        static const std::string log_path = log.string();
        if (std::FILE* f = std::fopen(log_path.c_str(), "w")) {
            rt::set_log_file(f);  // open until the process ends
            rt::set_log_path(log_path.c_str());
        } else {
            std::fprintf(stderr, "mkdd: the log %s can't be written: only here\n", log_path.c_str());
        }
    }
    rt::init({std::span(qilin::gen::kFunctions, qilin::gen::kFunctionCount),
              std::span(qilin::gen::kReplaced, qilin::gen::kReplacedCount)});
    rt::provide(mkdd::os_replacements());
    rt::provide(mkdd::dvd_replacements());
    rt::provide(mkdd::vi_replacements());
    rt::provide(mkdd::thread_replacements());
    rt::provide(mkdd::sc_replacements());
    rt::provide(mkdd::pad_replacements());
    rt::provide(mkdd::card_replacements());
    rt::provide(mkdd::exi_replacements());
    rt::provide(mkdd::ar_replacements());
    mkdd::init_hardware();
    if (bench_dff) {  // then out at once: the GPU thread is waiting, and exit's destructors would wait for it
        const int code = mkdd::bench_recording(bench_dff, bench_times);
        std::fflush(nullptr);
        std::_Exit(code);
    }
    if (render_dff) {
        // A frame captured with MKDD_DFF, replayed through the recorder and drawn by the
        // renderer with no game running: our picture of the very commands Dolphin's FIFO player
        // draws from the same file (tools/check/picture_check.py compares the two), starting as
        // the player starts and looping as it loops (replay_dff).
        qilin::host::Display::Options o;
        o.title = "Mario Kart: Double Dash!!";
        o.headless = true;
        o.width = 1280, o.height = 720;
        o.picture.scaling = qilin::host::Scaling::integer;  // a pixel for a pixel
        o.picture.smooth = false;
        qilin::host::Display display(o);
        unsigned scale = 1;
        if (const char* e = std::getenv("MKDD_SCALE")) scale = static_cast<unsigned>(std::clamp(std::atoi(e), 1, 8));
        mkdd::Renderer renderer(display.gpu(), scale, 1);
        renderer.set_true_color(std::getenv("MKDD_TRUE_COLOR") != nullptr);  // (the console's 18 bits unless asked)
        // Shown at its own size, a pixel for a pixel (4:3 for a 640x480 picture, in the middle of
        // the 1280x720 read back), and written so: Dolphin dumps the same pixels at its raw
        // resolution, and the two compare without either being resampled.
        uint32_t shown_w = 0, shown_h = 0;
        mkdd::replay_dff(render_dff, render_loops, [&](mkdd::gx::Frame& f) {
            f.captured = true;  // (MKDD_PROBE follows a pixel through a replayed frame too)
            renderer.render(f);
            for (const auto& c : f.copies)
                if (const qilin::host::Image* x = c.to_xfb ? renderer.xfb(c.dest) : nullptr) {
                    shown_w = x->width / scale, shown_h = x->height / scale;
                    display.present_image(x->image, x->view, x->width, x->height, float(shown_w) / float(shown_h), shown_h);
                }
        });
        if (!shown_w) rt::fatal(nullptr, "--render-dff: %s has no copy to the screen", render_dff);
        if (shown_w > 1280 || shown_h > 720) rt::fatal(nullptr, "--render-dff: a %ux%u picture doesn't fit the 1280x720 read back", shown_w, shown_h);
        const std::vector<uint8_t> px = display.read_back();
        FILE* fp = std::fopen(render_out, "wb");
        if (!fp) rt::fatal(nullptr, "--render-dff: %s can't be written", render_out);
        std::fprintf(fp, "P6\n%u %u\n255\n", shown_w, shown_h);
        const uint32_t x0 = (1280 - shown_w) / 2, y0 = (720 - shown_h) / 2;
        for (uint32_t y = 0; y < shown_h; ++y)
            for (uint32_t x = 0; x < shown_w; ++x) std::fwrite(&px[(size_t(y0 + y) * 1280 + x0 + x) * 4], 1, 3, fp);
        std::fclose(fp);
        std::fflush(nullptr);
        std::_Exit(0);
    }
    std::string restore_error;
    if (!mkdd::apply_save_restore(qilin::host::user_folder(), restore_error))
        rt::fatal(nullptr, "save recovery: %s (the restore request is restore-save.txt in the save folder)", restore_error.c_str());
    mkdd::load_game(folder);
    if (check_speaker) std::_Exit(check_speaker_decoder());

    const std::filesystem::path data = qilin::host::user_folder();
    mkdd::Settings settings = mkdd::load_settings(data);
    // (The Wii's settings, until the os layer answers the GameCube's own from settings.language,
    // settings.sound, settings.progressive and settings.eu_rgb60: English, 4:3.)
    mkdd::set_console(1, false, settings.sound);
    // The picture's shape: the game's own 4:3, until its cameras can be widened (enhance.hpp).
    float aspect = 4.0f / 3.0f;
    std::unique_ptr<qilin::host::Display> display;
    std::unique_ptr<mkdd::Renderer> renderer;
    std::unique_ptr<mkdd::Menu> menu;
    unsigned renderer_scale = 1;
    if (!headless) {
        qilin::host::Gpu::prefer(settings.gpu);
        qilin::host::Display::Options o;
        o.title = "Mario Kart: Double Dash!!";
        o.headless = offscreen;
        if (offscreen) o.width = 1280, o.height = 960;  // what MKDD_DUMP saves: the picture at twice its size
        o.fullscreen = settings.fullscreen;
        o.monitor = settings.monitor;
        o.picture = mkdd::picture_of(settings);
        display = std::make_unique<qilin::host::Display>(o);
        if (!display->mode_note().empty()) rt::log("%s", display->mode_note().c_str());
        if (auto* w = static_cast<SDL_Window*>(display->sdl_window()))
            if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)))
                rt::log("display: %dx%d at %.2f Hz%s", m->w, m->h, double(m->refresh_rate), settings.variable_refresh ? " (variable refresh)" : "");
        // The render resolution: auto is the smallest whole multiple whose 480 picture lines fill the
        // window as it is now (full screen: the screen). The renderer keeps it for the run.
        int window_w = 0, window_h = 0;
        if (auto* w = static_cast<SDL_Window*>(display->sdl_window())) SDL_GetWindowSizeInPixels(w, &window_w, &window_h);
        unsigned scale = mkdd::render_scale(settings, window_h);
        if (settings.scale == 0) rt::log("render resolution auto: %ux for a window %d pixels high", scale, window_h);
        if (mkdd::enhance::aspect_supported()) {
            aspect = mkdd::aspect_ratio(settings, window_h > 0 ? float(window_w) / float(window_h) : 4.0f / 3.0f);
            mkdd::enhance::set_aspect(aspect, settings.hud);
        }
        if (mkdd::enhance::frame_rate_supported()) mkdd::enhance::set_frame_rate(settings.frame_rate, 0);
        if (const char* e = std::getenv("MKDD_SCALE")) scale = static_cast<unsigned>(std::clamp(std::atoi(e), 1, 8));
        renderer = std::make_unique<mkdd::Renderer>(display->gpu(), scale, settings.anisotropy);
        static mkdd::Renderer* saving = nullptr;  // what was compiled since the last save, kept for next time
        saving = renderer.get();
        mkdd::g_before_exit = [] { saving->save_now(); };
        mkdd::gx::set_rendering(true);
        rt::log("drawing with %s, the EFB at %ux%s%s", display->device_name().c_str(), scale,
                display->gpu().dynamic_blend ? "; blend state set per draw (no pipeline to build for a new one)" : "",
                display->gpu().present_wait ? "; waits for each frame to reach the screen" : "");
        renderer_scale = scale;
        if (!offscreen) {
            menu = std::make_unique<mkdd::Menu>(*display, settings, data);
            menu->set_running_scale(scale);
        }
    }
    std::unique_ptr<qilin::host::AudioOut> audio;
    // (MKDD_AUDIO=1: sound without a window too, to test its queue; SDL_AUDIO_DRIVER=dummy plays to nowhere.)
    if ((!headless && !offscreen) || std::getenv("MKDD_AUDIO")) {
        // (With no window nothing reads SDL's events, so SDL mustn't turn Ctrl+C or a kill into one.)
        if (headless) SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        qilin::host::AudioOut::Options ao;
        ao.device = settings.sound_device, ao.output = settings.sound_output;
        audio = std::make_unique<qilin::host::AudioOut>(ao);
        rt::log("sound: %s, %s", audio->device_name().c_str(), audio->surround() ? "5.1 from the game's Dolby Surround" : "stereo");
        mkdd::set_audio_sink([](const int16_t* lr, size_t frames, unsigned rate, void* out) {
            static_cast<qilin::host::AudioOut*>(out)->push(lr, frames, rate);
        }, audio.get());
    }
    mkdd::run_on_host_thread([](void*) { mkdd::start_game(); }, nullptr);

    // Each port's controls, with each key's name looked up once (again when the menu changes them).
    mkdd::BoundControls bound[mkdd::kPlayers];
    const auto look_up_controls = [&] {
        for (unsigned p = 0; p < mkdd::kPlayers; ++p) {
            bound[p] = {};
            for (unsigned c = 0; c < mkdd::kControlCount; ++c)
                for (const mkdd::Binding& b : settings.players[p].controls[c]) {
                    int key = -1;
                    if (b.kind == mkdd::Binding::kKey && display && display->sdl_window() && (key = display->key_code(b.key.c_str())) < 0)
                        rt::fatal(nullptr, "mkdd.ini: [player%u] %s = key:%s, but there's no key called '%s'", p + 1,
                                  mkdd::control_name(static_cast<mkdd::Control>(c)), b.key.c_str(), b.key.c_str());
                    if (b.kind != mkdd::Binding::kPad) bound[p].has_keys = true;
                    bound[p].controls[c].push_back({b.kind, b.pad, key});
                }
        }
    };
    look_up_controls();
    // MKDD_INPUT, MKDD_INPUT2 ... MKDD_INPUT4: a script for each port (input_map.hpp).
    std::vector<mkdd::ScriptStep> scripts[mkdd::kPlayers];
    bool scripted[mkdd::kPlayers] = {};
    for (unsigned p = 0; p < mkdd::kPlayers; ++p) {
        const std::string variable = p ? "MKDD_INPUT" + std::to_string(p + 1) : std::string("MKDD_INPUT");
        const char* text = std::getenv(variable.c_str());
        std::string error;
        scripted[p] = text != nullptr;
        if (text && !mkdd::parse_input_script(text, scripts[p], error)) rt::fatal(nullptr, "%s: %s", variable.c_str(), error.c_str());
    }

    const char* dump = offscreen ? std::getenv("MKDD_DUMP") : nullptr;
    std::ofstream dump_index;
    if (dump) {
        std::filesystem::create_directories(dump);
        dump_index.open(std::filesystem::path(dump) / "frames.csv", std::ios::trunc);
        if (!dump_index) rt::fatal(nullptr, "cannot write capture frame index");
        dump_index << "file,vi_retrace\n";
    }
    const char* every_text = std::getenv("MKDD_DUMP_EVERY");
    const uint64_t every = every_text ? std::max<uint64_t>(1, std::strtoull(every_text, nullptr, 10)) : 60;
    const uint32_t dump_w = 1280, dump_h = 960;  // the offscreen target's size (above)
    uint8_t trigger_clicks[mkdd::kPlayers] = {};
    bool wait_control_release = false;
    uint64_t sequence = 0, shown = 0;
    std::vector<uint8_t> rgba;
    if (display) start_watchdog();
    for (;;) {
        g_main_passes.fetch_add(1, std::memory_order_relaxed);
        if (display) {
            std::array<std::string, qilin::host::kPlayerPlaces> chosen;
            for (unsigned p = 0; p < mkdd::kPlayers; ++p) chosen[p] = settings.players[p].controller;
            display->set_player_controllers(chosen);
            display->set_input_thresholds(settings.stick_threshold, settings.trigger_threshold);
            display->set_rumble_strength(settings.rumble_strength);
        }
        if (audio) audio->set_volume(settings.volume, settings.mute_unfocused && display && display->sdl_window() && !display->focused());
        if (display && !display->poll()) {
            rt::log("window closed");
            mkdd::before_exit();
            std::fflush(nullptr);
            std::_Exit(0);  // guest threads are parked on host threads: no static destructors
        }
        if (menu) {
            menu->tick();
            if (menu->quit_requested()) {
                rt::log("quit from the settings menu");
                mkdd::before_exit();
                std::fflush(nullptr);
                std::_Exit(0);
            }
            if (menu->take_controls_changed()) look_up_controls();
            if (menu->is_open()) {  // the game is parked: the last picture, with the menu over it
                wait_control_release = true;
                // Every controller at rest (and still plugged in: the game mustn't see them go).
                for (unsigned p = 0; p < mkdd::kPlayers; ++p) {
                    mkdd::GcPad rest = mkdd::gc_pad(int(p));
                    rest = mkdd::GcPad{rest.connected, 0, 0, 0, 0, 0, 0, 0};
                    mkdd::set_gc_pad(int(p), rest);
                }
                display->rumble_off();
                display->present_again();
                if (!settings.vsync) SDL_Delay(8);  // without vsync the menu needn't spin
                continue;
            }
        }
        if (renderer) renderer->set_anisotropy(settings.anisotropy);  // the menu's, once it's closed (unchanged: nothing to do)
        if (renderer) renderer->set_true_color(settings.true_color);
        {  // (without a display, --headless: the scripts alone)
            // The four controller ports (input.hpp), each from its player's bindings: the controller
            // at its place (display.hpp: by name, or in the order the controllers joined), and the
            // keyboard and mouse where the player has keys. The first port is always connected, as a
            // console with a controller in it; the others while their controller is, or with keys or
            // a script.
            mkdd::KeySource keys;
            if (display && display->sdl_window()) {
                keys.key_down = [&](int code) { return display->key_down(code); };
                keys.mouse = (display->buttons() & qilin::host::kPadClick) != 0;
            }
            const double now = mkdd::frame_count() / 60.0;  // game time: see the top
            mkdd::GcPad pads[mkdd::kPlayers];
            bool any = false;
            for (unsigned p = 0; p < mkdd::kPlayers; ++p) {
                const qilin::host::PadState pad = display ? display->pad_state(int(p)) : qilin::host::PadState{};
                pads[p] = mkdd::map_gc_pad(bound[p], pad, keys, settings.stick_deadzone, settings.trigger_threshold, trigger_clicks[p]);
                pads[p].connected = p == 0 || pad.connected || bound[p].has_keys || scripted[p];
                any = any || pads[p].buttons || pads[p].stick_x || pads[p].stick_y || pads[p].c_x || pads[p].c_y || pads[p].l || pads[p].r;
            }
            // After the menu: nothing reaches the game until everything is let go (the press that closed it).
            if (wait_control_release && !any) wait_control_release = false;
            for (unsigned p = 0; p < mkdd::kPlayers; ++p) {
                if (wait_control_release) pads[p] = mkdd::GcPad{pads[p].connected, 0, 0, 0, 0, 0, 0, 0};
                mkdd::apply_input_script(scripts[p], now, pads[p]);
                mkdd::set_gc_pad(int(p), pads[p]);
                if (display) display->rumble_pad(int(p), settings.players[p].rumble && mkdd::gc_rumble_wanted(int(p)));
            }
        }
        if (!renderer) {  // headless: nothing shows the frames, so they're only waited for, not converted
            if (audio) report_timing(nullptr, nullptr, audio.get());  // (MKDD_AUDIO: the sound queue's numbers)
            uint32_t xfb, w, h;
            mkdd::take_xfb(&sequence, 10, xfb, w, h);
            continue;
        }
        // MKDD_SPECIALIZE_FROM=<seconds of game time>: the ubershader until
        // then (for comparing the two on the same scene).
        static const double specialize_from = [] {
            const char* t = std::getenv("MKDD_SPECIALIZE_FROM");
            return t ? std::atof(t) : -1.0;
        }();
        if (specialize_from >= 0) renderer->set_specialization(mkdd::frame_count() / 60.0 >= specialize_from);
        // Draw what the guest recorded, then show what VI shows.
        for (int i = 0; i < 2; ++i)  // what's queued (two at most), then on to showing
            if (auto f = mkdd::gx::next_frame(i == 0 ? 4 : 0)) {
                renderer->render(*f);
                if (f->captured && display) {  // MKDD_DFF: our picture of the captured frame, beside it
                    for (const auto& c : f->copies)
                        if (const qilin::host::Image* x = c.to_xfb ? renderer->xfb(c.dest) : nullptr) {
                            display->present_image(x->image, x->view, x->width, x->height, aspect, x->height / renderer_scale);
                            const std::vector<uint8_t> px = display->read_back();
                            std::string name = std::getenv("MKDD_DFF");
                            name = name.substr(0, name.rfind('@')) + ".ppm";
                            if (FILE* fp = std::fopen(name.c_str(), "wb")) {
                                std::fprintf(fp, "P6\n%u %u\n255\n", dump_w, dump_h);
                                for (size_t k = 0; k + 3 < px.size(); k += 4) std::fwrite(&px[k], 1, 3, fp);
                                std::fclose(fp);
                                rt::log("MKDD_DFF: our picture of the frame: %s", name.c_str());
                            }
                        }
                }
                mkdd::gx::recycle_frame(std::move(f));
            }
            else break;
        // Every second: the retrace rate follows the display (it may have moved, or changed mode).
        if (display) {
            static auto next_match = std::chrono::steady_clock::time_point{};
            if (const auto t = std::chrono::steady_clock::now(); t >= next_match) {
                next_match = t + std::chrono::seconds(1);
                match_retrace_to_display(*display);
            }
        }
        // Every second: where a frame's time goes, for the F3 readout; every
        // 5 s, the log has it with how evenly frames reached the screen.
        if (display) report_timing(display.get(), menu.get(), audio.get());
        uint32_t xfb, w, h;
        if (!mkdd::take_xfb(&sequence, 0, xfb, w, h)) continue;
        if (const qilin::host::Image* x = xfb ? renderer->xfb(xfb) : nullptr) {
            display->present_image(x->image, x->view, x->width, x->height, aspect, h);
            steer_to_vblank();
            if (menu) menu->frame_shown();  // (F9: a capture takes it)
        } else {
            uint64_t s2 = sequence - 1;
            if (mkdd::take_frame(&s2, 0, rgba, w, h)) {
                display->present(w, h, rgba.data(), aspect, h);
                if (menu) menu->frame_shown();
            }
        }
        if (dump && ++shown % every == 0) {
            const std::vector<uint8_t> px = display->read_back();
            char name[512];
            std::snprintf(name, sizeof name, "%s/frame%05llu.ppm", dump, static_cast<unsigned long long>(shown / every));
            if (FILE* fp = std::fopen(name, "wb")) {
                std::fprintf(fp, "P6\n%u %u\n255\n", dump_w, dump_h);
                for (size_t i = 0; i + 3 < px.size(); i += 4) std::fwrite(&px[i], 1, 3, fp);
                std::fclose(fp);
                dump_index << std::filesystem::path(name).filename().string() << ',' << sequence << '\n';
                dump_index.flush();
                if (!dump_index) rt::fatal(nullptr, "capture frame index write failed");
                rt::log("dumped %s at %.1f s of game time", name, mkdd::frame_count() / 60.0);
            }
        }
    }
}
