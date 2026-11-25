#include "later.hpp"

#include <map>
#include <utility>

#include "os.hpp"

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

}  // namespace mkdd
