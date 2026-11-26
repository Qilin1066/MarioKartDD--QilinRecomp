// Development: a pilot for runs without a player (pilot.hpp). A race can't be
// driven by a script of timed presses, so the first port's stick can follow a
// path instead: the positions a kart passed through, taken from the game
// itself.
//   MKDD_PILOT_RECORD=<file>[:kart]  writes the kart's position (x z, a line every 10 frames) while a
//                                    race runs: kart 1 in a Grand Prix is one of the game's own drivers
//   MKDD_PILOT=<file>[,<file>...]    steers port 1's kart (kart 0) along such a path, closed into a lap;
//                                    the script still holds A. A file whose first line is `# course <hex>`
//                                    (the ECourseID the recording's `# race` line names) is taken for that
//                                    course; one without, for the race whose start it passes near. A race
//                                    with neither follows kart 1 live, along the trail it leaves (`follow`
//                                    in place of the files: always)
//   MKDD_PILOT_STEPS=<steps>         plays port 1 by the game's state instead of the clock (menus come up
//                                    seconds earlier or later from run to run, and a race ends when it
//                                    ends); MKDD_INPUT is then ignored for that port. Steps are separated
//                                    by ';' and taken in order, each `value:presses`:
//       value    <address>[><offset>...]=<n> or !<n>, as MKDD_WATCH reads it (hexadecimal): the step
//                begins half a second after the value is (or stops being) n; or +<seconds>: that long
//                after the step before it began
//       presses  MKDD_INPUT's items without times (A, Start, Stick:0:-110 ...), comma-separated, pressed
//                one after the other; `-` for none. After the last, `*` repeats it every 2.5 s until the
//                next step begins, `~` holds it but for a moment every 2.5 s (driving, and still a press
//                for whatever screen follows the race)
// Positions are read through the supported build's addresses (names from the disc's map):
// RaceMgr::sRaceManager, KartCtrl's instance, its kart bodies and their position.
#include "pilot.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "input_map.hpp"
#include "qilin/rt/system.hpp"

namespace mkdd {
namespace rt = qilin::rt;

namespace {
constexpr uint32_t kRaceManager = 0x803CB7E8u, kKartCtrl = 0x803CC588u, kBodies = 0xA0, kBodyPosition = 0x23C;
constexpr uint32_t kRaceCourse = 0x803CB6A8u;  // GameAudio::Parameters::sRaceCourse: the race's ECourseID

bool kart_position(uint32_t kart, float& x, float& z) {
    const uint32_t ctrl = rt::peek32(kKartCtrl);
    if (rt::peek32(kRaceManager) == 0 || ctrl < 0x80000000u || ctrl >= 0x81800000u) return false;
    const uint32_t body = rt::peek32(ctrl + kBodies + 4 * kart);
    if (body < 0x80000000u || body >= 0x81800000u) return false;
    const auto f = [](uint32_t a) { return std::bit_cast<float>(rt::peek32(a)); };
    x = f(body + kBodyPosition), z = f(body + kBodyPosition + 8);
    return true;
}
}  // namespace

namespace {
struct Step {
    std::vector<uint32_t> path;  // empty: `wait` seconds after the step before
    double wait = 0;
    uint32_t value = 0;
    bool differs = false;
    std::vector<ScriptStep> presses;
    char mode = 0;  // '*', '~' or 0
    std::string text;
};
bool holds(const Step& s, uint32_t frames) {
    if (s.path.empty()) return frames >= s.wait * 60.0;
    uint32_t a = s.path[0];
    const auto in_memory = [](uint32_t x) { return x >= 0x80000000u && x < 0x81800000u - 4; };
    if (!in_memory(a)) return false;
    for (size_t i = 1; i < s.path.size(); ++i) {
        a = rt::peek32(a) + s.path[i];
        if (!in_memory(a)) return false;
    }
    return (rt::peek32(a) == s.value) != s.differs;
}
std::vector<Step> parse_steps(std::string text) {
    std::vector<Step> out;
    while (!text.empty()) {
        const size_t semi = text.find(';');
        const std::string item = text.substr(0, semi);
        text = semi == std::string::npos ? "" : text.substr(semi + 1);
        if (item.empty()) continue;
        const auto bad = [&](const char* why) { rt::fatal(nullptr, "MKDD_PILOT_STEPS: \"%s\": %s", item.c_str(), why); };
        Step step;
        step.text = item;
        const char* at = item.c_str();
        if (*at == '+') {
            char* rest = nullptr;
            step.wait = std::strtod(at + 1, &rest);
            if (rest == at + 1 || *rest != ':') bad("expected +seconds:presses");
            at = rest + 1;
        }
        while (step.wait == 0) {
            char* rest = nullptr;
            step.path.push_back(static_cast<uint32_t>(std::strtoul(at, &rest, 16)));
            if (rest == at) bad("expected address[>offset...]=value:presses, in hexadecimal");
            at = rest + 1;
            if (*rest == '>') continue;
            if (*rest != '=' && *rest != '!') bad("expected = or ! and a value");
            step.differs = *rest == '!';
            step.value = static_cast<uint32_t>(std::strtoul(at, &rest, 16));
            if (rest == at || *rest != ':') bad("expected a value, then : and the presses");
            at = rest + 1;
            break;
        }
        std::string presses = at;
        if (!presses.empty() && (presses.back() == '*' || presses.back() == '~')) step.mode = presses.back(), presses.pop_back();
        if (presses != "-") {
            std::string script, error;
            for (size_t from = 0, n = 0; from <= presses.size(); ++n) {
                const size_t comma = std::min(presses.find(',', from), presses.size());
                script += (n ? "," : "") + presses.substr(from, comma - from) + "@" + std::to_string(n * 0.6);
                from = comma + 1;
            }
            if (!parse_input_script(script, step.presses, error)) bad(error.c_str());
        }
        out.push_back(std::move(step));
    }
    return out;
}
}  // namespace

// With steps, the path is followed only during a step that drives (`~`): the screens after a
// race still have a kart and a held A, and backing out of "being stuck" there would press B.
bool g_driving = true;

// Port 1's pad by MKDD_PILOT_STEPS.
bool pilot_pad(GcPad& pad) {
    static const char* text = std::getenv("MKDD_PILOT_STEPS");
    if (!text) return false;
    g_driving = false;
    static const std::vector<Step> steps = parse_steps(text);
    static size_t next = 0;        // the step waited for; the one before it is being played
    static uint32_t frames = 0;    // since that one began
    pad = GcPad{true, 0, 0, 0, 0, 0, 0, 0};
    if (next < steps.size() && holds(steps[next], frames)) {
        rt::log("pilot: step %zu (%s)", next + 1, steps[next].text.c_str());
        ++next;
        frames = 0;
    }
    if (next == 0) return true;
    const Step& s = steps[next - 1];
    g_driving = s.mode == '~';
    const double t = frames++ / 60.0 - 0.5;
    if (s.presses.empty() || t < 0) return true;
    const double last = s.presses.back().at;
    if (t < last || !s.mode) {
        apply_input_script(s.presses, t, pad);
    } else {
        const double phase = std::fmod(t - last, 2.5);
        if (phase < (s.mode == '~' ? 2.2 : 0.2)) apply_input_script(s.presses, last, pad);
    }
    return true;
}

void pilot_frame() {
    static const char* spec = std::getenv("MKDD_PILOT_RECORD");
    if (!spec) return;
    static uint32_t kart = 0, frame = 0;
    static FILE* out = [] {
        std::string name = spec;
        if (const size_t colon = name.rfind(':'); colon != std::string::npos && colon > 1 && name.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
            kart = static_cast<uint32_t>(std::atoi(name.c_str() + colon + 1));
            name.resize(colon);
        }
        FILE* f = std::fopen(name.c_str(), "w");
        if (!f) rt::fatal(nullptr, "MKDD_PILOT_RECORD: can't write %s", name.c_str());
        return f;
    }();
    float x, z;
    static bool racing = false;
    const bool now = kart_position(kart, x, z);
    if (now && !racing) std::fprintf(out, "# race course %X\n", rt::peek32(kRaceCourse)), frame = 0;  // (line n of a race is its frame 10 n)
    racing = now;
    if (!now || ++frame % 10) return;
    std::fprintf(out, "%.1f %.1f\n", x, z);
    std::fflush(out);
}
// The first port's stick, if the pilot has it: set in the status PADRead has just written.
void pilot_status(uint32_t status) {
    static const char* name = std::getenv("MKDD_PILOT");
    if (!name) return;
    struct Point {
        float x, z;
    };
    struct Path {
        std::vector<Point> points;
        uint32_t course = 0;  // its ECourseID, from a first line `# course <hex>`; 0: not said
    };
    // (Several files, comma-separated: one per course. A race takes the one for its course, or else the one
    // that passes nearest its start; with none for the course and none near, it follows a computer driver.)
    static const std::vector<Path> paths = [] {
        std::vector<Path> all;
        for (std::string names = name; !names.empty();) {
            const size_t comma = names.find(',');
            const std::string one = names.substr(0, comma);
            names = comma == std::string::npos ? "" : names.substr(comma + 1);
            if (one == "follow") continue;  // (no file: follow kart 1 on every course)
            Path out;
            FILE* f = std::fopen(one.c_str(), "r");
            if (!f) rt::fatal(nullptr, "MKDD_PILOT: can't read %s", one.c_str());
            if (std::fscanf(f, " # course %x", &out.course) != 1) out.course = 0, std::rewind(f);
            for (Point p; std::fscanf(f, "%f %f", &p.x, &p.z) == 2;) out.points.push_back(p);
            std::fclose(f);
            if (out.points.size() < 8) rt::fatal(nullptr, "MKDD_PILOT: %s holds no path", one.c_str());
            all.push_back(std::move(out));
        }
        return all;
    }();
    // Following: the positions the computer driver in kart 1 passed through this race, every 10 frames.
    static std::vector<Point> trail;
    static bool following = false;
    static uint32_t trail_frames = 0;
    static size_t course = 0;
    static const float sign = [] { const char* e = std::getenv("MKDD_PILOT_SIGN"); return e && *e == '-' ? -1.0f : 1.0f; }();
    static float last_x = 0, last_z = 0, heading_x = 0, heading_z = 0;
    static size_t nearest = 0;
    static uint32_t reversing = 0;  // frames left of backing out
    static bool racing = false;
    float x, z;
    if (!g_driving || !kart_position(0, x, z)) {
        racing = false;
        return;
    }
    if (racing && (x - last_x) * (x - last_x) + (z - last_z) * (z - last_z) > 2000.0f * 2000.0f) racing = false;  // put somewhere else: a restart
    if (!racing) {  // a new race: the path for its course, else the one passing nearest, else follow kart 1
        const uint32_t id = rt::peek32(kRaceCourse);
        float best = 1e30f;
        following = true;
        for (size_t c = 0; c < paths.size(); ++c)
            if (paths[c].course && paths[c].course == id) course = c, following = false, best = 0;
        for (size_t c = 0; c < paths.size() && best > 0; ++c)
            for (const Point& p : paths[c].points)
                if (const float d = (p.x - x) * (p.x - x) + (p.z - z) * (p.z - z); !paths[c].course && d < best) best = d, course = c;
        if (best > 0 && best < 800.0f * 800.0f) following = false;
        trail.clear();
        trail_frames = 0;
        rt::log("pilot: a race on course %X: %s", id, following ? "no path for it, following kart 1" : "along a recorded path");
    }
    if (following && trail_frames++ % 10 == 0) {
        float fx, fz;
        if (kart_position(1, fx, fz) &&
            (trail.empty() || (fx - trail.back().x) * (fx - trail.back().x) + (fz - trail.back().z) * (fz - trail.back().z) > 50.0f * 50.0f))
            trail.push_back({fx, fz});
    }
    if (following && trail.empty()) return;
    const std::vector<Point>& path = following ? trail : paths[course].points;
    const size_t size = path.size();
    const auto distance2 = [&](size_t i) { return (path[i].x - x) * (path[i].x - x) + (path[i].z - z) * (path[i].z - z); };
    // (A recorded path is a closed lap; the trail is open, ending where kart 1 is now.)
    const auto ahead = [&](size_t n) { return following ? std::min(nearest + n, size - 1) : (nearest + n) % size; };
    if (!racing) {  // a new race: wherever the kart is on the path (following: the trail's start)
        nearest = 0;
        for (size_t i = 1; i < size && !following; ++i)
            if (distance2(i) < distance2(nearest)) nearest = i;
        heading_x = heading_z = 0;
    } else {  // along from where it was, never back
        for (size_t step = 1; step <= 6; ++step)
            if (distance2(ahead(step)) < distance2(nearest)) nearest = ahead(step), step = 0;
        const float dx = x - last_x, dz = z - last_z;
        if (dx * dx + dz * dz > 1.0f && !reversing) heading_x = dx, heading_z = dz;  // (not while backing out)
    }
    racing = true;
    last_x = x, last_z = z;
    // Stuck (against a wall, in the scenery): hardly moved in a second with A held. Back out for a
    // second and a half with B, then go on.
    static float mark_x = 0, mark_z = 0;
    static uint32_t since_mark = 0;
    const uint16_t buttons = rt::peek16(status);
    if (reversing) {
        --reversing;
        rt::poke16(status, static_cast<uint16_t>((buttons & ~0x0100u) | 0x0200u));
        rt::poke8(status + 2, 0);
        mark_x = x, mark_z = z, since_mark = 0;
        return;
    }
    if (!(buttons & 0x0100) || ++since_mark >= 60) {
        if ((buttons & 0x0100) && since_mark >= 60 && (x - mark_x) * (x - mark_x) + (z - mark_z) * (z - mark_z) < 150.0f * 150.0f)
            reversing = 90;
        mark_x = x, mark_z = z, since_mark = 0;
    }
    const Point& target = path[ahead(4)];
    const float tx = target.x - x, tz = target.z - z;
    if (heading_x == 0 && heading_z == 0) return;  // not moving yet: straight on
    const float angle = std::atan2(heading_x * tz - heading_z * tx, heading_x * tx + heading_z * tz);
    const long stick = std::lround(std::clamp(sign * angle * 200.0f, -100.0f, 100.0f));
    rt::poke8(status + 2, static_cast<uint8_t>(static_cast<int8_t>(stick)));
}

}  // namespace mkdd
