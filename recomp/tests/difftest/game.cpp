// qilin_game_difftest: the game's own functions, lifted with the library plan,
// each run against the reference interpreter (ref.cpp) from the same states.
//
//   qilin_game_difftest <main.dol> [--trials n] [--seed n] [--steps n] [--only address] [--data symbols-data.txt]
//   qilin_game_difftest <main.dol> --thp movie.thp [--frames n] [--reps n]
//
// Where qilin_difftest checks each instruction alone, this checks what the
// lifter makes of whole functions: its register model across calls, branches,
// jump tables, tail calls, falling into the next function, single mode and
// the write-backs on every way out. The interpreter runs first, on the DOL's
// image with everything around it made to look like a running game's memory:
// a heap of 64-byte objects whose first word is mostly a vtable of the image,
// the rest pointers to other objects, small numbers, floats and function
// addresses; BSS and the stack likewise; and the first megabyte of the address
// space backed too, so that a null or small "pointer" reads something on both
// sides instead of ending the run. Each function's arguments follow its own
// first use of them: a register it loads through gets an object, one it
// compares with a small constant a number near it.
//
// A run counts when it comes back to its caller (or traps) having stayed
// inside that memory and inside the program; then the lifted function runs
// from the same state, and every register and every byte of memory must
// agree. Code the plan replaces or drops, and the few functions the lifter
// refuses, return at once on both sides (the lifted thunk calls a hook that
// does nothing here). Runs that go astray are skipped and counted.
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
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
#include <tuple>
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
// of function addresses (a stand-in vtable) and the stack. And [0, kLow).
constexpr uint32_t kLo = 0x80000000u, kHi = 0x80800000u, kSize = kHi - kLo;
constexpr uint32_t kLow = 0x100000u;
constexpr uint32_t kHeapLo = 0x80480000u, kHeapHi = 0x80700000u, kObject = 0x40u;
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
enum : uint8_t { kStart = 1, kNoBody = 2, kTable = 4 };  // kNoBody: replaced, dropped or not liftable: returns at once
struct Program {
    uint32_t text_lo = 0, text_hi = 0;
    std::vector<Insn> code;
    std::vector<uint8_t> flags;
    std::map<uint32_t, std::vector<uint32_t>> tables;  // by the bctr's address: targets, sorted

    bool in_text(uint32_t a) const { return a >= text_lo && a < text_hi && a % 4 == 0 && code[(a - text_lo) / 4].valid(); }
    size_t index(uint32_t a) const { return (a - text_lo) / 4; }
    bool function(uint32_t a) const { return a >= text_lo && a < text_hi && a % 4 == 0 && (flags[index(a)] & kStart); }
};

// What a replaced function does, for the few the movie decoder needs (--thp); the same on both
// sides. Null: every replaced function returns at once.
void (*g_native)(uint32_t function, uint32_t* gpr, uint8_t* mem) = nullptr;

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
        const bool no_body = (p.flags[i] & kStart) && (p.flags[i] & kNoBody);
        const Insn in = no_body ? ppc::decode(0x4E800020u) : p.code[i];  // (a blr)
        if (!no_body) executed.push_back(static_cast<uint32_t>(i));
        else if (g_native) g_native(pc, s.gpr, s.mem);
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
void on_trap(rt::Cpu&, uint32_t pc, const char* why) {
    if (std::string_view(why).starts_with("not lifted")) return;  // a function the lifter refuses: returns at once here
    throw Trapped{pc};
}
void on_replaced(rt::Cpu& c, uint32_t f) {  // replaced or dropped code: returns at once here
    if (g_native) g_native(f, c.r, c.mem);
}
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

// What a function first does with each GPR, read off its instructions in
// address order (not by path: a hint for choosing arguments, nothing more).
enum class Use : uint8_t { Unknown, Pointer, Index, Number };
struct ArgHints {
    Use use[32] = {};
    uint32_t bound[32] = {};  // Number: the constant it's compared with
};
ArgHints hints_of(const Function& f, const Cfg& cfg) {
    ArgHints h;
    bool seen[32] = {};
    for (uint32_t a : f.insns) {
        const Insn in = cfg.insn(a);
        const ppc::Spec& s = ppc::spec(in.op);
        if ((s.flags & ppc::kLk) && in.lk()) break;  // a call: the registers are the callee's from here
        bool d_form = false, has_rb = false, has_ra = false, has_rd = false;
        for (ppc::Opnd o : s.operands) {
            d_form |= o == ppc::Opnd::d_rA0 || o == ppc::Opnd::psq_d_rA0;
            has_rb |= o == ppc::Opnd::rB;
            has_ra |= o == ppc::Opnd::rA || o == ppc::Opnd::rA0;
            has_rd |= o == ppc::Opnd::rD;
        }
        has_ra |= d_form;
        const bool memory = (s.flags & (ppc::kLoad | ppc::kStore)) || in.op == Op::dcbz;
        auto first = [&](unsigned r, Use u, uint32_t bound = 0) {
            if (!seen[r]) h.use[r] = u, h.bound[r] = bound;
            seen[r] = true;
        };
        if (memory) {
            if (in.ra() != 0) first(in.ra(), Use::Pointer);
            if (!d_form && has_rb) first(in.rb(), in.ra() == 0 ? Use::Pointer : Use::Index);
        } else if ((in.op == Op::cmpi || in.op == Op::cmpli) && in.uimm() < 0x400) {
            first(in.ra(), Use::Number, in.uimm());
        }
        if (has_rd) seen[in.rd()] = true;
        if (has_ra) seen[in.ra()] = true;
        if (has_rb) seen[in.rb()] = true;
    }
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: qilin_game_difftest <main.dol> [--trials n] [--seed n] [--steps n] [--only address] [--data symbols-data.txt]");
        return 2;
    }
    unsigned trials = 8, max_steps = 20000;
    uint32_t seed = 20261125u, only = 0;
    std::string data_path, thp_path;
    unsigned thp_frames = 3, thp_reps = 5;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view o = argv[i];
        if (o == "--trials") trials = parse_u32(argv[i + 1], 10);
        else if (o == "--seed") seed = parse_u32(argv[i + 1], 10);
        else if (o == "--steps") max_steps = parse_u32(argv[i + 1], 10);
        else if (o == "--only") only = parse_u32(argv[i + 1], 16);
        else if (o == "--data") data_path = argv[i + 1];
        else if (o == "--thp") thp_path = argv[i + 1];
        else if (o == "--frames") thp_frames = parse_u32(argv[i + 1], 10);
        else if (o == "--reps") thp_reps = parse_u32(argv[i + 1], 10);
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
    for (const DolSection& s : dol->sections)
        if (s.text) {
            if (!p.text_lo) p.text_lo = s.address;
            p.text_hi = s.end();
        }
    p.code.resize((p.text_hi - p.text_lo) / 4);
    p.flags.assign(p.code.size(), 0);
    for (uint32_t a = p.text_lo; a < p.text_hi; a += 4) p.code[p.index(a)] = cfg.insn(a);
    for (unsigned k = 0; k < gen::kReplacedCount; ++k) p.flags[p.index(gen::kReplaced[k].address)] |= kNoBody;
    std::vector<uint32_t> lifted;  // the functions under test
    std::map<uint32_t, ArgHints> hints;
    size_t lifted_insns = 0;
    for (const auto& [start, f] : cfg.functions()) {
        uint8_t& flags = p.flags[p.index(start)];
        flags |= kStart;
        if (!(flags & kNoBody) && !lift(lift_input(cfg, f)).ok) flags |= kNoBody;
        if (flags & kNoBody) continue;
        lifted.push_back(start);
        hints[start] = hints_of(f, cfg);
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

    // The image's vtables, as CodeWarrior lays them out: a type-info pointer (or 0), a zero, then
    // function addresses; an object's first word points at the first of those two words.
    std::vector<uint32_t> vtables, globals;
    for (const DolSection& s : dol->sections) {
        if (s.text) continue;
        for (uint32_t a = (s.address + 3) & ~3u; a + 16 <= s.end(); a += 4) {
            if (dol->u32(a + 4) == 0 && p.function(dol->u32(a + 8)) && p.function(dol->u32(a + 12)) && !p.function(dol->u32(a))) vtables.push_back(a);
            if ((a & 31) == 0) globals.push_back(a);
        }
    }
    // Named data, if given: better targets for a pointer into the image than every 32nd byte.
    if (!data_path.empty()) {
        std::ifstream f(data_path);
        std::string l;
        std::vector<uint32_t> named;
        while (std::getline(f, l))
            if (l.size() > 8 && l[0] != '#' && l[8] == ' ') {
                const uint32_t a = parse_u32(l.substr(0, 8), 16);
                if (dol->mapped(a, 4) && !cfg.in_text(a)) named.push_back(a);
            }
        if (named.empty()) {
            std::println(stderr, "game difftest: no data symbols in {}", data_path);
            return 1;
        }
        globals = std::move(named);
    }
    if (vtables.empty()) vtables.push_back(kVtableLo);

    // The memory every run starts from: [0, kLow) and [kLo, kHi).
    std::mt19937 soup_rng(seed);
    std::vector<uint8_t> pristine(kSize, 0), pristine_low(kLow, 0);
    auto put = [&](uint32_t a, uint32_t v) {
        uint8_t* at = a < kLow ? pristine_low.data() + a : pristine.data() + (a - kLo);
        for (unsigned k = 0; k < 4; ++k) at[k] = static_cast<uint8_t>(v >> (24 - 8 * k));
    };
    const uint32_t objects = (kHeapHi - kHeapLo - 0x200) / kObject;
    auto object = [&](std::mt19937& r) { return kHeapLo + 0x100 + (r() % objects) * kObject; };
    auto heap_pointer = [&](std::mt19937& r) { return kHeapLo + 0x100 + (r() % ((kHeapHi - kHeapLo - 0x200) / 8)) * 8; };
    auto global = [&](std::mt19937& r) { return globals[r() % globals.size()]; };
    auto soup = [&](std::mt19937& r) -> uint32_t {
        const unsigned k = r() % 100;
        if (k < 48) return object(r);
        if (k < 51) return heap_pointer(r);
        if (k < 55) return kVtableLo + (r() % ((kVtableHi - kVtableLo - 0x400) / 4)) * 4;
        if (k < 56) return lifted[r() % lifted.size()];
        if (k < 60) return global(r);
        if (k < 80) return r() % 32;
        if (k < 92) return 0;
        if (k < 96) return std::bit_cast<uint32_t>(std::ldexp(static_cast<float>(r()) / 4294967296.0f - 0.5f, static_cast<int>(r() % 12)));
        return static_cast<uint32_t>(r());
    };
    for (uint32_t a = 0; a < kLow; a += 4) put(a, soup(soup_rng));
    for (uint32_t a = kLo; a < dol->base(); a += 4) put(a, soup(soup_rng));
    for (uint32_t a = dol->image_end() & ~3u; a < kHi; a += 4) put(a, soup(soup_rng));
    for (uint32_t a = (dol->bss_address + 3) & ~3u; a + 4 <= dol->bss_address + dol->bss_size; a += 4) put(a, soup(soup_rng));
    for (uint32_t a = kVtableLo; a < kVtableHi; a += 4) put(a, lifted[soup_rng() % lifted.size()]);
    for (uint32_t a = kHeapLo + 0x100; a + kObject <= kHeapHi; a += kObject)  // most objects have a vtable, most of those a real one
        if (const unsigned k = soup_rng() % 10; k < 7) put(a, k < 5 ? vtables[soup_rng() % vtables.size()] : kVtableLo + (soup_rng() % 0x8000) * 4);
    std::copy(dol->image().begin(), dol->image().begin() + (dol->bss_address - dol->base()), pristine.begin() + (dol->base() - kLo));
    for (const DolSection& s : dol->sections)  // (the small-data sections lie inside the BSS range)
        std::copy_n(dol->image().begin() + (s.address - dol->base()), s.size, pristine.begin() + (s.address - kLo));

    rt::GuestMemory lifted_memory, ref_memory;
    for (rt::GuestMemory* m : {&lifted_memory, &ref_memory})
        if (!m->reserve() || !m->commit(kLo, kSize) || !m->commit(0, kLow)) {
            std::println(stderr, "game difftest: cannot reserve guest memory");
            return 1;
        }
    uint8_t* const lm = lifted_memory.base() + kLo;
    uint8_t* const rm = ref_memory.base() + kLo;
    for (uint8_t* base : {lifted_memory.base(), ref_memory.base()}) {
        std::memcpy(base + kLo, pristine.data(), kSize);
        std::memcpy(base, pristine_low.data(), kLow);
    }
    std::vector<uint8_t> dirty((kSize + kLow) / kPage, 0);  // the main range's pages, then the low one's
    std::signal(SIGSEGV, on_crash);

    // --thp: the SDK's movie decoder on real frames, the heaviest paired-single code the game
    // has: THPInit, then THPVideoDecode on the first frames of a .thp file, on the reference and
    // lifted; everything must agree after each call. Then the lifted decode alone, timed.
    if (!thp_path.empty()) {
        constexpr uint32_t kThpInit = 0x800F7CC8u, kThpVideoDecode = 0x800F0800u;  // GM4E01 (the symbols port's names)
        constexpr uint32_t kLc = 0xE0000000u, kLcSize = 0x4000u;                    // the decoder works in the locked cache
        constexpr uint32_t kFile = 0x80500000u, kY = 0x80580000u, kU = 0x80600000u, kV = 0x80640000u, kWork = 0x80680000u;
        std::ifstream f(thp_path, std::ios::binary);
        const std::vector<uint8_t> movie{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        auto be = [&](size_t o) { return o + 4 <= movie.size() ? read_be32(movie.data() + o) : 0u; };
        if (movie.size() < 0x40 || be(0) != 0x54485000u) {
            std::println(stderr, "game difftest: {} isn't a THP movie", thp_path);
            return 1;
        }
        const uint32_t components = be(be(0x20));
        const uint32_t width = be(be(0x20) + 20), height = be(be(0x20) + 24);  // (the video component's info comes first)
        if (!lifted_memory.commit(kLc, kLcSize) || !ref_memory.commit(kLc, kLcSize) || components == 0 || components > 16) {
            std::println(stderr, "game difftest: cannot set up the movie");
            return 1;
        }
        rt::Cpu c;
        c.mem = lifted_memory.base();
        c.hooks = &kHooks;
        c.r[1] = kStackTop;
        c.r[2] = sda[0];
        c.r[13] = sda[1];
        ref::State s;
        std::memcpy(s.gpr, c.r, sizeof s.gpr);
        s.mem = ref_memory.base();
        s.lo = kLo, s.hi = kHi, s.low = kLow, s.extra = kLc, s.extra_size = kLcSize;
        // The OS functions the decoder leans on: HID2 says the locked cache is on, DCZeroRange
        // clears the work area, LCStoreData copies a finished row out of the locked cache.
        g_native = [](uint32_t function, uint32_t* gpr, uint8_t* mem) {
            if (function == 0x800A96CCu) gpr[3] = 0xB0000000u;                                           // PPCMfhid2
            else if (function == 0x800E715Cu) std::memset(mem + (gpr[3] & ~31u), 0, (gpr[4] + (gpr[3] & 31u) + 31u) & ~31u);  // DCZeroRange
            else if (function == 0x800E7330u) std::memmove(mem + gpr[3], mem + gpr[4], gpr[5]);         // LCStoreData
        };
        using Clock = std::chrono::steady_clock;
        std::vector<uint32_t> executed;
        double lifted_seconds = 0;
        auto lifted_call = [&](uint32_t fn, std::initializer_list<uint32_t> args) {
            unsigned r = 3;
            for (uint32_t a : args) c.r[r++] = a;
            c.lr = kReturn;
            g_current = fn;
            const auto t0 = Clock::now();
            on_call(c, fn);
            lifted_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        };
        auto both = [&](const char* what, uint32_t fn, std::initializer_list<uint32_t> args) {
            unsigned r = 3;
            for (uint32_t a : args) s.gpr[r++] = a;
            s.lr = kReturn;
            executed.clear();
            const Outcome end = interpret(s, p, fn, 2000000000u, executed);
            if (end.end != End::Returned) {
                std::println(stderr, "FAIL {}: the reference stopped at {:08X}: {}{}", what, end.pc, end.why, s.fault ? std::format(" ({:08X})", s.fault_at) : "");
                return false;
            }
            lifted_call(fn, args);
            std::vector<std::string> diffs;
            for (unsigned n = 0; n < 32; ++n) {
                if (c.r[n] != s.gpr[n]) diffs.push_back(std::format("r{} {:08X} vs {:08X}", n, c.r[n], s.gpr[n]));
                if (std::bit_cast<uint64_t>(c.ps0[n]) != s.ps0[n]) diffs.push_back(std::format("f{}.ps0 {:016X} vs {:016X}", n, std::bit_cast<uint64_t>(c.ps0[n]), s.ps0[n]));
                if (std::bit_cast<uint64_t>(c.ps1[n]) != s.ps1[n]) diffs.push_back(std::format("f{}.ps1 {:016X} vs {:016X}", n, std::bit_cast<uint64_t>(c.ps1[n]), s.ps1[n]));
            }
            if (c.cr != s.cr || c.ctr != s.ctr || c.lr != s.lr || c.xer() != (s.xer & 0xE000007Fu)) diffs.push_back("cr, ctr, lr or xer");
            for (unsigned g = 0; g < 8; ++g)
                if (c.gqr[g] != s.gqr[g]) diffs.push_back(std::format("gqr{}", g));
            for (const auto& [base, size] : {std::pair{kLo, kSize}, std::pair{0u, kLow}, std::pair{kLc, kLcSize}})
                if (std::memcmp(lifted_memory.base() + base, ref_memory.base() + base, size) != 0) {
                    uint32_t i = 0;
                    while (lifted_memory.base()[base + i] == ref_memory.base()[base + i]) ++i;
                    diffs.push_back(std::format("memory at {:08X}", base + i));
                }
            std::println("{}: {} instructions, returns {:08X}, {} differences", what, executed.size(), s.gpr[3], diffs.size());
            for (size_t d = 0; d < diffs.size() && d < 12; ++d) std::println(stderr, "    {} (lifted vs reference)", diffs[d]);
            return diffs.empty();
        };
        if (!both("THPInit", kThpInit, {})) return 1;
        std::vector<std::pair<uint32_t, uint32_t>> images;  // file offset and size of each frame's picture
        for (uint32_t at = be(0x28), size = be(0x18), n = 0; n < thp_frames && at + size <= movie.size(); ++n) {
            images.emplace_back(at + 8 + 4 * components, be(at + 8));
            const uint32_t next = be(at);
            at += size;
            size = next;
        }
        auto load = [&](const std::pair<uint32_t, uint32_t>& image) {
            for (uint8_t* base : {lifted_memory.base(), ref_memory.base()}) std::memcpy(base + kFile, movie.data() + image.first, image.second);
        };
        for (size_t n = 0; n < images.size(); ++n) {
            load(images[n]);
            if (!both(std::format("frame {}", n).c_str(), kThpVideoDecode, {kFile, kY, kU, kV, kWork})) return 1;
        }
        double best = 1e9;
        for (unsigned rep = 0; rep < thp_reps; ++rep) {
            double total = 0;
            for (const auto& image : images) {
                std::memcpy(lifted_memory.base() + kFile, movie.data() + image.first, image.second);
                lifted_call(kThpVideoDecode, {kFile, kY, kU, kV, kWork});
                total += lifted_seconds;
            }
            best = std::min(best, total / static_cast<double>(images.size()));
        }
        const double blocks = (width / 16) * (height / 16) * 6.0;
        std::println("THP {}x{}: lifted decode {:.3f} ms a frame ({:.0f} ns an 8x8 block), best of {} passes over {} frames",
                     width, height, best * 1e3, best * 1e9 / blocks, thp_reps, images.size());
        return 0;
    }

    std::vector<uint8_t> covered(p.code.size(), 0);
    std::vector<uint32_t> executed;
    std::map<std::string, size_t> skipped;
    std::map<uint32_t, size_t> faults;  // wild accesses, by the address's top byte
    size_t compared = 0, tested = 0, failures = 0, runs = 0, traps = 0;
    for (const uint32_t start : lifted) {
        if (only && start != only) continue;
        const auto entry = std::lower_bound(gen::kFunctions, gen::kFunctions + gen::kFunctionCount, start,
                                            [](const gen::FunctionEntry& f, uint32_t a) { return f.address < a; });
        if (entry == gen::kFunctions + gen::kFunctionCount || entry->address != start) {
            std::println(stderr, "game difftest: fn_{:08X} is missing from the lifted table", start);
            return 1;
        }
        const ArgHints& hint = hints.at(start);
        bool any = false;
        for (unsigned trial = 0; trial < trials; ++trial, ++runs) {
            std::mt19937 rng(seed ^ (start * 2654435761u) ^ (trial * 40503u));
            rt::Cpu c;
            // Arguments: what the function first does with each register decides (three trials in
            // four); otherwise mostly objects, in a mix that changes with the trial.
            const unsigned pointers = 40 + 15 * (trial % 5);
            for (unsigned r = 0; r < 32; ++r) {
                const unsigned k = rng() % 100;
                const Use use = trial % 4 == 3 ? Use::Unknown : hint.use[r];
                if (use == Use::Pointer) c.r[r] = k < 80 ? object(rng) : k < 90 ? global(rng) : k < 97 ? heap_pointer(rng) : 0;
                else if (use == Use::Index) c.r[r] = (rng() % 16) * (k < 50 ? 4 : 1);
                else if (use == Use::Number) c.r[r] = k < 90 ? rng() % (hint.bound[r] + 2) : static_cast<uint32_t>(rng());
                else c.r[r] = k < pointers ? object(rng) : k < pointers + 15 ? rng() % 64 : k < pointers + 20 ? 0 : soup(rng);
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
            s.low = kLow;
            s.dirty = dirty.data();
            executed.clear();
            const Outcome ref_end = interpret(s, p, start, max_steps, executed);
            auto restore = [&](bool lifted_too) {
                for (size_t page = 0; page < dirty.size(); ++page)
                    if (dirty[page]) {
                        const bool low = page >= kSize / kPage;
                        const size_t at = low ? (page - kSize / kPage) * kPage : page * kPage;
                        const uint8_t* from = (low ? pristine_low.data() : pristine.data()) + at;
                        std::memcpy((low ? ref_memory.base() : rm) + at, from, kPage);
                        if (lifted_too) std::memcpy((low ? lifted_memory.base() : lm) + at, from, kPage);
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
                if (s.fault) ++faults[s.fault_at >> 24];
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
            bool memory_differs = false;
            for (const auto& [l, r, size, base] : {std::tuple{lm, rm, kSize, kLo}, std::tuple{lifted_memory.base(), ref_memory.base(), kLow, 0u}})
                if (std::memcmp(l, r, size) != 0) {
                    size_t i = 0;
                    while (l[i] == r[i]) ++i;
                    diffs.push_back(std::format("memory at {:08X}: {:02X} vs {:02X}", base + static_cast<uint32_t>(i), l[i], r[i]));
                    memory_differs = true;
                }
            if (memory_differs || !diffs.empty()) {
                std::memcpy(lm, pristine.data(), kSize);
                std::memcpy(lifted_memory.base(), pristine_low.data(), kLow);
            }
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
    std::string where;
    for (const auto& [top, n] : faults)
        if (n * 50 >= runs) where += std::format(" {:02X}xxxxxx {},", top, n);
    if (!where.empty()) std::println("  wild accesses by address:{}", where.substr(0, where.size() - 1));
    return failures ? 1 : 0;
}
