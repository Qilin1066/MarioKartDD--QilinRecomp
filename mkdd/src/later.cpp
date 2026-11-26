#include "later.hpp"

#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "os.hpp"
#include "video.hpp"

namespace mkdd {
namespace rt = qilin::rt;

namespace {

// By time due, then by the order asked.
std::map<std::pair<uint64_t, uint64_t>, std::function<void(rt::Cpu&)>> g_due;
uint64_t g_sequence = 0;
bool g_source = false;

void poll() {
    const uint64_t now = time_base();
    while (!g_due.empty() && g_due.begin()->first.first <= now) {
        post_interrupt(std::move(g_due.begin()->second));
        g_due.erase(g_due.begin());
    }
}
uint64_t deadline() { return g_due.empty() ? UINT64_MAX : g_due.begin()->first.first; }

}  // namespace

void run_after(uint64_t ticks, std::function<void(rt::Cpu&)> work) {
    if (!g_source) {
        g_source = true;
        add_timer_source(poll, deadline);
    }
    g_due.emplace(std::pair{time_base() + ticks, g_sequence++}, std::move(work));
}

uint64_t ticks_from_us(uint64_t us) { return us * kTimerHz / 1000000; }

bool devices_log() {
    static const bool on = [] { const char* e = std::getenv("MKDD_DEVICES_LOG"); return e && *e && *e != '0'; }();
    return on;
}

void watch_guest_values() {
    struct Watch {
        std::string text;
        std::vector<uint32_t> path;  // an address, then offsets from the pointer found there
        uint32_t size, last;
        bool seen;
    };
    static std::vector<Watch> watches = [] {
        std::vector<Watch> out;
        const char* e = std::getenv("MKDD_WATCH");
        for (std::string text = e ? e : ""; !text.empty();) {
            const size_t comma = text.find(',');
            const std::string item = text.substr(0, comma);
            text = comma == std::string::npos ? "" : text.substr(comma + 1);
            Watch w{item, {}, 4, 0, false};
            const char* at = item.c_str();
            for (;;) {
                char* rest = nullptr;
                w.path.push_back(static_cast<uint32_t>(std::strtoul(at, &rest, 16)));
                if (rest == at) rt::fatal(nullptr, "MKDD_WATCH: \"%s\" isn't address[>offset...][:size], in hexadecimal", item.c_str());
                at = rest + 1;
                if (*rest == '>') continue;
                if (*rest == ':') w.size = static_cast<uint32_t>(std::strtoul(at, nullptr, 10));
                break;
            }
            if (w.size != 1 && w.size != 2 && w.size != 4) rt::fatal(nullptr, "MKDD_WATCH: \"%s\": a size is 1, 2 or 4", item.c_str());
            out.push_back(std::move(w));
        }
        return out;
    }();
    const auto in_memory = [](uint32_t a) { return a >= 0x80000000u && a < 0x81800000u - 4; };
    for (Watch& w : watches) {
        uint32_t a = w.path[0], v = 0xDEAD0000u;  // (a path through a null or wild pointer reads as this)
        bool ok = in_memory(a);
        for (size_t i = 1; ok && i < w.path.size(); ++i) {
            a = rt::peek32(a) + w.path[i];
            ok = in_memory(a);
        }
        if (ok) v = w.size == 1 ? *rt::guest_ptr(a) : w.size == 2 ? rt::peek16(a) : rt::peek32(a);
        if (w.seen && v == w.last) continue;
        rt::log("watch %.2f: [%s] = %08X", frame_count() / 60.0, w.text.c_str(), v);
        w.last = v, w.seen = true;
    }
}

}  // namespace mkdd
