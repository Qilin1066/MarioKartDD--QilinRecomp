// The response-time probe (latency.hpp).
#include "latency.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "qilin/rt/system.hpp"

namespace mkdd::latency {
namespace rt = qilin::rt;

namespace {
// Pseudo-buttons of the probe's own: the main stick held down or up (menus that go by the stick).
constexpr uint16_t kStickDown = 0x8000, kStickUp = 0x4000, kStick = kStickDown | kStickUp;
void apply(GcPad& pad, uint16_t held) {
    if (held & kStickDown) pad.stick_y = -100;
    if (held & kStickUp) pad.stick_y = 100;
    pad.buttons |= held & ~kStick;
}
struct Config {
    bool on = false;
    double start = 0;
    uint16_t fixed = 0;  // a button named, or 0: Down and Up in turn
    unsigned ahead = 2;  // frames after the one being read's: the first that can show it
};
const Config& config() {
    static const Config c = [] {
        Config c;
        const char* e = std::getenv("MKDD_LATENCY");
        if (!e) return c;
        c.on = true;
        c.start = std::atof(e);
        if (const char* colon = std::strchr(e, ':')) {
            const std::string b = colon + 1;
            c.fixed = b == "Stick" ? kStick : b == "A" ? kGcA : b == "B" ? kGcB : b == "Start" ? kGcStart : b == "Left" ? kGcLeft : b == "Right" ? kGcRight
                    : b == "Up" ? kGcUp : kGcDown;
        }
        if (const char* k = std::getenv("MKDD_LATENCY_AHEAD")) c.ahead = unsigned(std::atoi(k));
        return c;
    }();
    return c;
}

// One press, followed (times in host ns; 0 not yet).
struct Press {
    uint16_t bit = 0;
    uint64_t at = 0, sampled = 0, read = 0, handed = 0, drawn = 0, presented = 0;
    uint64_t frame = 0;  // the first frame that can show it
};
std::mutex g_lock;
Press g_press;              // the one in flight
uint64_t g_next = 0, g_release = 0;
unsigned g_count = 0;
bool g_down_next = true;
std::atomic<uint16_t> g_held{0};
std::atomic<uint64_t> g_handed{0}, g_pictures{0};
std::atomic<bool> g_following{false};
std::atomic<uint64_t> g_check_tail{0};  // (MKDD_LATENCY_CHECK: pictures looked at after a press's)
std::vector<Press> g_done;
std::mt19937_64 g_random{0x1A7E9C};

void finish_locked() {
    const Press& p = g_press;
    const auto ms = [](uint64_t a, uint64_t b) { return double(b - a) / 1e6; };
    rt::log("latency: press %u (%s): sampled +%.2f, read +%.2f, frame %llu handed +%.2f, drawn +%.2f, presented +%.2f: %.2f ms in all", g_count,
            p.bit == kGcDown ? "Down" : p.bit == kGcUp ? "Up" : "button", ms(p.at, p.sampled), ms(p.sampled, p.read),
            static_cast<unsigned long long>(p.frame), ms(p.read, p.handed), ms(p.handed, p.drawn), ms(p.drawn, p.presented), ms(p.at, p.presented));
    g_done.push_back(p);
    if (g_done.size() % 20 == 0) {
        const auto stat = [&](const char* what, auto get) {
            double sum = 0, lo = 1e30, hi = 0;
            for (const Press& q : g_done) {
                const double v = get(q);
                sum += v, lo = std::min(lo, v), hi = std::max(hi, v);
            }
            rt::log("latency:   %-10s %6.2f ms on average (%.2f to %.2f)", what, sum / double(g_done.size()), lo, hi);
        };
        rt::log("latency: over %zu presses", g_done.size());
        stat("sampled", [&](const Press& q) { return ms(q.at, q.sampled); });
        stat("read", [&](const Press& q) { return ms(q.sampled, q.read); });
        stat("handed", [&](const Press& q) { return ms(q.read, q.handed); });
        stat("drawn", [&](const Press& q) { return ms(q.handed, q.drawn); });
        stat("presented", [&](const Press& q) { return ms(q.drawn, q.presented); });
        stat("in all", [&](const Press& q) { return ms(q.at, q.presented); });
    }
    g_press = {};
    g_following.store(false, std::memory_order_relaxed);
    g_check_tail.store(g_pictures.load(std::memory_order_relaxed) + 3, std::memory_order_relaxed);
}
}  // namespace

bool enabled() { return config().on; }
uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t pictures() { return g_pictures.load(std::memory_order_relaxed); }

void sample(GcPad& pad, double game_seconds) {
    const Config& c = config();
    if (!c.on || game_seconds < c.start) return;
    const uint64_t now = now_ns();
    std::lock_guard l(g_lock);
    if (g_next == 0) g_next = now + 1'000'000'000;
    if (g_release && now >= g_release) g_release = 0, g_held.store(0, std::memory_order_relaxed);
    if (!g_press.at && !g_following.load(std::memory_order_relaxed) && now >= g_next) {
        // A moment the input changed, anywhere in the time since the last look: as a thumb's would.
        g_press = {};
        g_press.bit = c.fixed == kStick ? (g_down_next ? kStickDown : kStickUp) : c.fixed ? c.fixed : g_down_next ? kGcDown : kGcUp;
        g_down_next = !g_down_next;
        g_press.at = g_next;
        g_press.sampled = now;
        ++g_count;
        g_held.store(g_press.bit, std::memory_order_relaxed);
        g_release = now + 200'000'000;
        g_next += 1'000'000'000 + std::uniform_int_distribution<uint64_t>(0, 50'000'000)(g_random);
        g_following.store(true, std::memory_order_relaxed);
    }
    apply(pad, g_held.load(std::memory_order_relaxed));
}
void probe_apply(GcPad& pad) { apply(pad, g_held.load(std::memory_order_relaxed)); }

void pad_read(const GcPad& pad) {
    if (!g_following.load(std::memory_order_relaxed)) return;
    std::lock_guard l(g_lock);
    const bool seen = g_press.bit == kStickDown ? pad.stick_y < -50 : g_press.bit == kStickUp ? pad.stick_y > 50 : (pad.buttons & g_press.bit) != 0;
    if (g_press.read || !seen) return;
    g_press.read = now_ns();
    g_press.frame = g_handed.load(std::memory_order_relaxed) + config().ahead;
}

void frame_handed(uint64_t number) {
    g_handed.store(number, std::memory_order_relaxed);
    if (!g_following.load(std::memory_order_relaxed)) return;
    std::lock_guard l(g_lock);
    if (g_press.read && !g_press.handed && number >= g_press.frame) g_press.handed = now_ns();
}
void frame_drawn(uint64_t number) {
    if (!g_following.load(std::memory_order_relaxed)) return;
    std::lock_guard l(g_lock);
    if (g_press.handed && !g_press.drawn && number >= g_press.frame) g_press.drawn = now_ns();
}
void presented(uint64_t newest) {
    g_pictures.fetch_add(1, std::memory_order_relaxed);
    if (!g_following.load(std::memory_order_relaxed)) return;
    std::lock_guard l(g_lock);
    if (g_press.drawn && newest >= g_press.frame) {
        g_press.presented = now_ns();
        rt::log("latency: press %u shown in picture %llu", g_count, static_cast<unsigned long long>(g_pictures.load()));
        finish_locked();
    }
}

}  // namespace mkdd::latency

namespace mkdd::latency {
bool checking() {
    static const bool on = std::getenv("MKDD_LATENCY_CHECK") != nullptr;
    return on && (g_following.load(std::memory_order_relaxed) || g_pictures.load(std::memory_order_relaxed) <= g_check_tail.load(std::memory_order_relaxed));
}
void check_picture(const uint8_t* rgba, uint32_t width, uint32_t height) {
    static uint64_t last[64] = {};
    uint64_t cells[64] = {};
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            uint64_t& h = cells[(y * 8 / height) * 8 + x * 8 / width];
            uint32_t px;
            std::memcpy(&px, rgba + (size_t{y} * width + x) * 4, 4);
            h = (h ^ px) * 0x100000001B3ull;
        }
    std::string changed;
    for (unsigned i = 0; i < 64; ++i) changed += cells[i] != last[i] ? 'x' : '.';
    std::memcpy(last, cells, sizeof last);
    rt::log("latency check: picture %llu, cells changed %s", static_cast<unsigned long long>(g_pictures.load()), changed.c_str());
}
}  // namespace mkdd::latency

namespace mkdd::latency {
bool hashing() {
    static const bool on = std::getenv("MKDD_PICTURE_HASHES") != nullptr;
    return on;
}
void hash_picture(const std::vector<uint8_t>& rgba, double game_seconds, const char* what) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const uint8_t b : rgba) h = (h ^ b) * 0x100000001B3ull;
    static uint64_t n = 0;
    rt::log("picture %llu (%s) at %.2f s: %016llX", static_cast<unsigned long long>(++n), what, game_seconds, static_cast<unsigned long long>(h));
}
}  // namespace mkdd::latency
