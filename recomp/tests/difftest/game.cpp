// qilin_game_difftest: the game's own functions, lifted with no plan, each run
// against the reference interpreter (ref.cpp) from the same random states.
//
//   qilin_game_difftest <main.dol> [--plan plan.tsv] [--trials n] [--seed n] [--steps n] [--only address]
//
// With the library plan, only the functions it lifts are tested, and a run that
// reaches replaced or dropped code is skipped, as one that reaches a stub is.
//
// Where qilin_difftest checks each instruction alone, this checks what the
// lifter makes of whole functions: its register model across calls, branches,
// jump tables, tail calls, falling into the next function, single mode and
// the write-backs on every way out. The interpreter runs first, on the DOL's
// image with everything around it (BSS, a heap, the stack) filled with
// plausible values: pointers into the heap, small numbers, floats, and
// "vtables" of function addresses. A run counts when it comes back to its
// caller (or traps) having stayed inside that memory and inside lifted code;
// then the lifted function runs from the same state, and every register and
// every byte of memory must agree. Runs that go astray (a wild pointer, a
// hardware register, a function that isn't lifted, too long) are skipped and
// counted: random arguments take most functions somewhere, not everywhere.
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "cfg.hpp"
#include "dol.hpp"
#include "functions.hpp"
#include "lift.hpp"
#include "qilin/rt/cpu.hpp"
#include "qilin/rt/memory.hpp"
#include "ref.hpp"

using namespace qilin;
using ppc::Insn;
using ppc::Op;

namespace {

// The test's memory: the image low in it, then soup. Above the heap, a zone
// of function addresses (what a vtable pointer in the soup points at) and the
// stack.
constexpr uint32_t kLo = 0x80000000u, kHi = 0x80800000u, kSize = kHi - kLo;
constexpr uint32_t kHeapLo = 0x80480000u, kHeapHi = 0x80700000u;
constexpr uint32_t kVtableLo = 0x80700000u, kVtableHi = 0x80740000u;
constexpr uint32_t kStackTop = 0x807F0000u;
constexpr uint32_t kReturn = 0xFFFFFFFCu;  // LR on entry: back to the test
constexpr uint32_t kPage = 4096;

struct Trapped {
    uint32_t pc;
};
struct Astray {  // the lifted code did what the interpreter's run rules out
    std::string what;
};

// What the interpreter needs to know of the program, by text word.
enum : uint8_t { kStart = 1, kStub = 2, kTable = 4 };
struct Program {
    uint32_t text_lo = 0, text_hi = 0;
    std::vector<Insn> code;
    std::vector<uint8_t> flags;
    std::map<uint32_t, std::vector<uint32_t>> tables;  // by the bctr's address: targets, sorted
    uint32_t image_lo = 0, image_hi = 0;

    bool in_text(uint32_t a) const { return a >= text_lo && a < text_hi && a % 4 == 0 && code[(a - text_lo) / 4].valid(); }
    size_t index(uint32_t a) const { return (a - text_lo) / 4; }
    bool function(uint32_t a) const { return a >= text_lo && a < text_hi && a % 4 == 0 && (flags[index(a)] & kStart); }
};

enum class End { Returned, Trap, Skipped, Broken };
struct Outcome {
    End end = End::Skipped;
    const char* why = "";
    uint32_t pc = 0;
};

Outcome interpret(ref::State& s, const Program& p, uint32_t entry, unsigned max_steps, std::vector<uint32_t>& executed) {
    std::vector<uint32_t> returns;  // where each call in progress comes back to
    uint32_t pc = entry;
    auto skip = [&](const char* why) { return Outcome{End::Skipped, why, pc}; };
    for (unsigned steps = 0; steps < max_steps; ++steps) {
        if (!p.in_text(pc)) return skip("left the code");
        const size_t i = p.index(pc);
        if ((p.flags[i] & (kStart | kStub)) == (kStart | kStub)) return skip("reached a function that isn't lifted");
        const Insn in = p.code[i];
        executed.push_back(static_cast<uint32_t>(i));
        const unsigned bo = in.rd(), bi = in.ra();
        // A call: the lifted code calls the function and carries on at `back`.
        auto call = [&](uint32_t target, uint32_t back) {
            returns.push_back(back);
            pc = target;
        };
        switch (in.op) {
        case Op::b: {
            const uint32_t target = in.branch_target(pc);
            if (!in.lk()) {
                pc = target;
                break;
            }
            s.lr = pc + 4;
            if (target == pc + 4) pc += 4;  // reads the PC
            else if (!p.in_text(target)) return skip("called outside the code");
            else call(target, pc + 4);
            break;
        }
        case Op::bc: {
            const uint32_t target = in.branch_target(pc);
            const bool taken = ref::branch_taken(s, bo, bi);
            if (in.lk()) s.lr = pc + 4;
            if (!taken || target == pc + 4) pc += 4;
            else if (in.lk()) call(target, pc + 4);
            else pc = target;
            break;
        }
        case Op::bclr: {
            const uint32_t target = s.lr & ~3u;
            const bool taken = ref::branch_taken(s, bo, bi);
            if (in.lk()) s.lr = pc + 4;
            if (!taken) {
                pc += 4;
            } else if (in.lk()) {
                if (!p.function(target)) return skip("indirect call to something that isn't a function");
                call(target, pc + 4);
            } else if (returns.empty()) {
                // A lifted function returns to its caller whatever LR holds; only a run where
                // LR says the same is one the two can be compared on.
                if (target != kReturn) return skip("returned somewhere else");
                return {End::Returned, "", pc};
            } else {
                if (target != returns.back()) return skip("returned somewhere else");
                pc = returns.back();
                returns.pop_back();
            }
            break;
        }
        case Op::bcctr: {
            if (p.flags[i] & kTable) {  // a switch: an address outside its table traps
                const std::vector<uint32_t>& t = p.tables.at(pc);
                if (!std::ranges::binary_search(t, s.ctr)) return {End::Trap, "", pc};
                pc = s.ctr;
                break;
            }
            const bool taken = ref::branch_taken(s, bo, bi);
            const uint32_t target = s.ctr & ~3u;
            if (in.lk()) s.lr = pc + 4;
            if (!taken) {
                pc += 4;
            } else {
                if (!p.function(target)) return skip("indirect call to something that isn't a function");
                if (in.lk()) call(target, pc + 4);
                else pc = target;  // a tail call
            }
            break;
        }
        default:
            if (!ref::step(s, in, pc)) return {End::Broken, "the reference doesn't know an instruction of a lifted function", pc};
            if (s.fault) return skip("touched memory outside the test's");
            if (s.trapped) return {End::Trap, "", pc};
            pc += 4;
            break;
        }
        if (returns.size() > 160) return skip("calls nested too deeply");
    }
    return skip("ran too long");
}

// ---- the lifted side's hooks
uint32_t slow_read(rt::Cpu&, uint32_t a, unsigned) { throw Astray{std::format("loaded from {:08X}", a)}; }
void slow_write(rt::Cpu&, uint32_t a, uint32_t, unsigned) { throw Astray{std::format("stored to {:08X}", a)}; }
void on_call(rt::Cpu& c, uint32_t target) {
    const gen::FunctionEntry* b = gen::kFunctions;
    const gen::FunctionEntry* e = b + gen::kFunctionCount;
    const auto it = std::lower_bound(b, e, target, [](const gen::FunctionEntry& f, uint32_t a) { return f.address < a; });
    if (it == e || it->address != target) throw Astray{std::format("called {:08X}, which isn't a function", target)};
    it->fn(c);
}
void on_trap(rt::Cpu&, uint32_t pc, const char*) { throw Trapped{pc}; }
void on_replaced(rt::Cpu&, uint32_t f) { throw Astray{std::format("called replaced function {:08X}", f)}; }
const rt::CpuHooks kHooks{slow_read, slow_write, on_call, on_trap, on_replaced, [](rt::Cpu&) {}};

uint32_t g_current = 0;
unsigned g_trial = 0;
void on_crash(int) {
    std::fprintf(stderr, "game difftest: the lifted fn_%08X crashed the host (trial %u): rerun with --only %08X\n", g_current, g_trial, g_current);
    std::_Exit(3);
}

uint32_t parse_u32(std::string_view s, int radix) {
    uint32_t v = 0;
    if (s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2), radix = 16;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, radix);
    if (ec != std::errc{} || end != s.data() + s.size()) {
        std::println(stderr, "game difftest: not a number: {}", s);
        std::exit(2);
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: qilin_game_difftest <main.dol> [--plan plan.tsv] [--trials n] [--seed n] [--steps n] [--only address]");
        return 2;
    }
    unsigned trials = 8, max_steps = 20000;
    uint32_t seed = 20261125u, only = 0;
    std::string plan_path;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view o = argv[i];
        if (o == "--trials") trials = parse_u32(argv[i + 1], 10);
        else if (o == "--seed") seed = parse_u32(argv[i + 1], 10);
        else if (o == "--steps") max_steps = parse_u32(argv[i + 1], 10);
        else if (o == "--only") only = parse_u32(argv[i + 1], 16);
        else if (o == "--plan") plan_path = argv[i + 1];
        else {
            std::println(stderr, "game difftest: unknown option {}", o);
            return 2;
        }
    }
    const auto dol = Dol::load(argv[1]);
    if (!dol) {
        std::println(stderr, "game difftest: {}", dol.error());
        return 1;
    }
    if (dol->image_end() > kHeapLo || dol->base() < kLo) {
        std::println(stderr, "game difftest: the image doesn't fit below the test's heap");
        return 1;
    }
    Cfg cfg(*dol);
    cfg.run();
    if (cfg.functions().size() != gen::kFunctionCount) {
        std::println(stderr, "game difftest: built from another main.dol ({} functions here, {} lifted)", cfg.functions().size(), gen::kFunctionCount);
        return 1;
    }

    Program p;
    p.image_lo = dol->base();
    p.image_hi = dol->image_end();
    for (const DolSection& s : dol->sections)
        if (s.text) {
            if (!p.text_lo) p.text_lo = s.address;
            p.text_hi = s.end();
        }
    p.code.resize((p.text_hi - p.text_lo) / 4);
    p.flags.assign(p.code.size(), 0);
    for (uint32_t a = p.text_lo; a < p.text_hi; a += 4) p.code[p.index(a)] = cfg.insn(a);
    // The plan's ranges that aren't lifted (start, end, plan, library per line; the narrowest range decides).
    struct Range {
        uint32_t start, end;
        bool lift;
    };
    std::vector<Range> plan;
    if (!plan_path.empty()) {
        std::ifstream f(plan_path);
        std::string l;
        while (std::getline(f, l)) {
            if (l.empty() || l[0] == '#') continue;
            const size_t t1 = l.find('	'), t2 = l.find('	', t1 + 1), t3 = l.find('	', t2 + 1);
            if (t3 == std::string::npos) continue;
            plan.push_back({parse_u32(l.substr(0, t1), 16), parse_u32(l.substr(t1 + 1, t2 - t1 - 1), 16), l.substr(t2 + 1, t3 - t2 - 1) == "lift"});
        }
        if (plan.empty()) {
            std::println(stderr, "game difftest: cannot read the plan {}", plan_path);
            return 1;
        }
    }
    auto planned_out = [&](uint32_t a) {
        const Range* best = nullptr;
        for (const Range& r : plan)
            if (a >= r.start && a < r.end && (!best || r.end - r.start < best->end - best->start)) best = &r;
        return !plan.empty() && (!best || !best->lift);
    };
    std::vector<uint32_t> lifted;  // the functions under test
    size_t lifted_insns = 0;
    for (const auto& [start, f] : cfg.functions()) {
        p.flags[p.index(start)] |= kStart;
        if (planned_out(start) || !lift(lift_input(cfg, f)).ok) {
            p.flags[p.index(start)] |= kStub;
            continue;
        }
        lifted.push_back(start);
        lifted_insns += f.insns.size();
        for (const JumpTable& t : f.jtabs) {
            p.flags[p.index(t.at)] |= kTable;
            p.tables[t.at] = t.targets;
        }
    }

    // The small-data bases, as the program's start-up sets them (lis rN; ori rN).
    uint32_t sda[2] = {0, 0};  // r2, r13
    for (uint32_t a = p.text_lo; a + 4 < p.text_hi && !(sda[0] && sda[1]); a += 4) {
        const Insn hi = p.code[p.index(a)], lo = p.code[p.index(a + 4)];
        for (unsigned k = 0; k < 2; ++k) {
            const unsigned r = k ? 13 : 2;
            if (!sda[k] && hi.op == Op::addis && hi.rd() == r && hi.ra() == 0 && lo.op == Op::ori && lo.ra() == r && lo.rd() == r)
                sda[k] = (hi.uimm() << 16) | lo.uimm();
        }
    }

    // The memory every run starts from.
    std::mt19937 soup_rng(seed);
    std::vector<uint8_t> pristine(kSize, 0);
    auto put = [&](uint32_t a, uint32_t v) {
        for (unsigned k = 0; k < 4; ++k) pristine[a - kLo + k] = static_cast<uint8_t>(v >> (24 - 8 * k));
    };
    auto heap_pointer = [&](std::mt19937& r) { return kHeapLo + 0x100 + (r() % ((kHeapHi - kHeapLo - 0x200) / 8)) * 8; };
    auto soup = [&](std::mt19937& r) -> uint32_t {
        const unsigned k = r() % 100;
        if (k < 45) return heap_pointer(r);
        if (k < 50) return kVtableLo + (r() % ((kVtableHi - kVtableLo - 0x400) / 4)) * 4;
        if (k < 65) return r() % 32;
        if (k < 75) return 0;
        if (k < 87) return std::bit_cast<uint32_t>(std::ldexp(static_cast<float>(r()) / 4294967296.0f - 0.5f, static_cast<int>(r() % 12)));
        return static_cast<uint32_t>(r());
    };
    for (uint32_t a = dol->image_end() & ~3u; a < kHi; a += 4) put(a, soup(soup_rng));
    for (uint32_t a = (dol->bss_address + 3) & ~3u; a + 4 <= dol->bss_address + dol->bss_size; a += 4) put(a, soup(soup_rng));
    for (uint32_t a = kVtableLo; a < kVtableHi; a += 4) put(a, lifted[soup_rng() % lifted.size()]);
    std::copy(dol->image().begin(), dol->image().begin() + (dol->bss_address - dol->base()), pristine.begin() + (dol->base() - kLo));
    for (const DolSection& s : dol->sections)  // (the small-data sections lie inside the BSS range)
        std::copy_n(dol->image().begin() + (s.address - dol->base()), s.size, pristine.begin() + (s.address - kLo));

    rt::GuestMemory lifted_memory, ref_memory;
    if (!lifted_memory.reserve() || !lifted_memory.commit(kLo, kSize) || !ref_memory.reserve() || !ref_memory.commit(kLo, kSize)) {
        std::println(stderr, "game difftest: cannot reserve guest memory");
        return 1;
    }
    uint8_t* const lm = lifted_memory.base() + kLo;
    uint8_t* const rm = ref_memory.base() + kLo;
    std::memcpy(lm, pristine.data(), kSize);
    std::memcpy(rm, pristine.data(), kSize);
    std::vector<uint8_t> dirty(kSize / kPage, 0);
    std::signal(SIGSEGV, on_crash);

    std::vector<uint8_t> covered(p.code.size(), 0);
    std::vector<uint32_t> executed;
    std::map<std::string, size_t> skipped;
    size_t compared = 0, tested = 0, failures = 0, runs = 0, traps = 0;
    for (const uint32_t start : lifted) {
        if (only && start != only) continue;
        const auto entry = std::lower_bound(gen::kFunctions, gen::kFunctions + gen::kFunctionCount, start,
                                            [](const gen::FunctionEntry& f, uint32_t a) { return f.address < a; });
        if (entry == gen::kFunctions + gen::kFunctionCount || entry->address != start) {
            std::println(stderr, "game difftest: fn_{:08X} is missing from the lifted table", start);
            return 1;
        }
        bool any = false;
        for (unsigned trial = 0; trial < trials; ++trial, ++runs) {
            std::mt19937 rng(seed ^ (start * 2654435761u) ^ (trial * 40503u));
            rt::Cpu c;
            // Arguments: mostly objects in the heap (every trial has a different mix).
            const unsigned pointers = 40 + 15 * (trial % 5);
            for (unsigned r = 0; r < 32; ++r) {
                const unsigned k = rng() % 100;
                c.r[r] = k < pointers ? heap_pointer(rng) : k < pointers + 15 ? rng() % 64 : k < pointers + 20 ? 0 : soup(rng);
            }
            c.r[1] = kStackTop - 0x800 - (rng() % 256) * 16;
            c.r[2] = sda[0];
            c.r[13] = sda[1];
            c.cr = static_cast<uint32_t>(rng());
            c.set_xer(static_cast<uint32_t>(rng()) & 0xE000007Fu);
            c.lr = kReturn;
            c.ctr = rng() % 4 ? rng() % 8 : static_cast<uint32_t>(rng());
            // GQR0 is the SDK's: floats. The others: any valid type and scale.
            for (unsigned g = 1; g < 8; ++g) {
                static const unsigned types[] = {0, 4, 5, 6, 7};
                c.gqr[g] = (static_cast<uint32_t>(rng()) & 0x3F003F00u) | types[rng() % 5] | (types[rng() % 5] << 16);
            }
            // Every other trial every FPR half is a single-precision value: single mode's entry check passes.
            uint64_t f0[32], f1[32];
            for (unsigned n = 0; n < 32; ++n)
                for (uint64_t* h : {&f0[n], &f1[n]}) {
                    const unsigned k = rng() % 16;
                    const double ordinary = std::ldexp(static_cast<double>(rng()) / 4294967296.0 - 0.5, static_cast<int>(rng() % 16) - 4);
                    if (trial % 2) *h = rt::fp::bits(rt::fp::load_single(k == 0 ? static_cast<uint32_t>(rng()) : std::bit_cast<uint32_t>(static_cast<float>(ordinary))));
                    else *h = k == 0 ? (uint64_t{static_cast<uint32_t>(rng())} << 32 | static_cast<uint32_t>(rng())) : k == 1 ? 0 : std::bit_cast<uint64_t>(ordinary);
                }
            for (unsigned n = 0; n < 32; ++n) c.ps0[n] = std::bit_cast<double>(f0[n]), c.ps1[n] = std::bit_cast<double>(f1[n]);

            ref::State s;
            std::memcpy(s.gpr, c.r, sizeof s.gpr);
            s.cr = c.cr;
            s.xer = c.xer();
            s.lr = c.lr;
            s.ctr = c.ctr;
            std::memcpy(s.gqr, c.gqr, sizeof s.gqr);
            std::memcpy(s.ps0, f0, sizeof f0);
            std::memcpy(s.ps1, f1, sizeof f1);
            s.mem = ref_memory.base();
            s.lo = kLo;
            s.hi = kHi;
            s.dirty = dirty.data();
            executed.clear();
            const Outcome ref_end = interpret(s, p, start, max_steps, executed);
            auto restore = [&](bool lifted_too) {
                for (size_t page = 0; page < dirty.size(); ++page)
                    if (dirty[page]) {
                        std::memcpy(rm + page * kPage, pristine.data() + page * kPage, kPage);
                        if (lifted_too) std::memcpy(lm + page * kPage, pristine.data() + page * kPage, kPage);
                        dirty[page] = 0;
                    }
            };
            if (ref_end.end == End::Broken) {
                std::println(stderr, "FAIL fn_{:08X}: {} at {:08X}", start, ref_end.why, ref_end.pc);
                ++failures;
                restore(false);
                break;
            }
            if (ref_end.end == End::Skipped) {
                ++skipped[ref_end.why];
                restore(false);
                continue;
            }

            c.mem = lifted_memory.base();
            c.hooks = &kHooks;
            g_current = start;
            g_trial = trial;
            bool trapped = false;
            uint32_t trap_pc = 0;
            std::vector<std::string> diffs;
            try {
                entry->fn(c);
            } catch (const Trapped& t) {
                trapped = true;
                trap_pc = t.pc;
            } catch (const Astray& a) {
                diffs.push_back("the lifted code " + a.what);
            }
            const bool ref_trapped = ref_end.end == End::Trap;
            if (trapped != ref_trapped) diffs.push_back(std::format("trap {} vs {} (at {:08X})", trapped, ref_trapped, trapped ? trap_pc : ref_end.pc));
            // (A tw's trap names its address on both sides; psq with a bad GQR can't happen here.)
            else if (trapped && trap_pc != ref_end.pc) diffs.push_back(std::format("trap at {:08X} vs {:08X}", trap_pc, ref_end.pc));
            for (unsigned r = 0; r < 32; ++r)
                if (c.r[r] != s.gpr[r]) diffs.push_back(std::format("r{} {:08X} vs {:08X}", r, c.r[r], s.gpr[r]));
            if (c.cr != s.cr) diffs.push_back(std::format("cr {:08X} vs {:08X}", c.cr, s.cr));
            if (c.xer() != (s.xer & 0xE000007Fu)) diffs.push_back(std::format("xer {:08X} vs {:08X}", c.xer(), s.xer & 0xE000007Fu));
            if (c.lr != s.lr) diffs.push_back(std::format("lr {:08X} vs {:08X}", c.lr, s.lr));
            if (c.ctr != s.ctr) diffs.push_back(std::format("ctr {:08X} vs {:08X}", c.ctr, s.ctr));
            for (unsigned n = 0; n < 32; ++n) {
                const uint64_t x0 = std::bit_cast<uint64_t>(c.ps0[n]), x1 = std::bit_cast<uint64_t>(c.ps1[n]);
                if (x0 != s.ps0[n]) diffs.push_back(std::format("f{}.ps0 {:016X} vs {:016X}", n, x0, s.ps0[n]));
                if (x1 != s.ps1[n]) diffs.push_back(std::format("f{}.ps1 {:016X} vs {:016X}", n, x1, s.ps1[n]));
            }
            for (unsigned g = 0; g < 8; ++g)
                if (c.gqr[g] != s.gqr[g]) diffs.push_back(std::format("gqr{} {:08X} vs {:08X}", g, c.gqr[g], s.gqr[g]));
            if (static_cast<bool>(c.reserve) != s.reserve || (s.reserve && c.reserved != s.reserve_addr)) diffs.push_back("reservation differs");
            const bool memory_differs = std::memcmp(lm, rm, kSize) != 0;
            if (memory_differs) {
                size_t i = 0;
                while (lm[i] == rm[i]) ++i;
                diffs.push_back(std::format("memory at {:08X}: {:02X} vs {:02X} (was {:02X})", kLo + i, lm[i], rm[i], pristine[i]));
            }
            if (memory_differs || !diffs.empty()) std::memcpy(lm, pristine.data(), kSize);
            restore(true);
            ++compared;
            traps += ref_trapped;
            any = true;
            for (uint32_t i : executed) covered[i] = 1;
            if (!diffs.empty()) {
                if (++failures <= 30) {
                    std::println(stderr, "FAIL fn_{:08X} trial {} (--only {:08X} --seed {}), {} instructions run:", start, trial, start, seed, executed.size());
                    for (size_t d = 0; d < diffs.size() && d < 12; ++d) std::println(stderr, "    {} (lifted vs reference)", diffs[d]);
                }
                break;  // one report per function
            }
        }
        tested += any;
    }
    size_t covered_insns = 0;
    for (uint8_t b : covered) covered_insns += b;
    std::println("game difftest: {} lifted functions, {} runs: {} compared ({} of them trap), in {} functions; "
                 "{} of {} lifted instructions run in compared runs ({}); {} functions differ",
                 only ? 1 : lifted.size(), runs, compared, traps, tested, covered_insns, lifted_insns, percent(covered_insns, lifted_insns, 1), failures);
    std::vector<std::pair<size_t, std::string>> by;
    for (const auto& [why, n] : skipped) by.emplace_back(n, why);
    std::ranges::sort(by, std::greater{});
    for (const auto& [n, why] : by) std::println("  {:7} runs skipped: {}", n, why);
    return failures ? 1 : 0;
}
