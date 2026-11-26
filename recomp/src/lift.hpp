// The lifter: one guest function to one C++ function over qilin::rt::Cpu
// (runtime/include/qilin/rt/cpu.hpp).
//
// Slice 1 (v0.4.0): integer arithmetic, logic, rotates and shifts, compares
// and the condition register, integer loads and stores (with update, indexed,
// byte-reversed, multiple, lwarx/stwcx.), dcbz and the cache hints, LR, CTR,
// XER and the GQRs, traps, and all branches and calls. Anything else (floating
// point, paired singles, privileged and system instructions, overflow-enable
// and string forms) makes the function a stub that traps with the reason when
// it's called, so nothing is skipped silently (docs/recompiler-core.md).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cfg.hpp"
#include "decoder.hpp"
#include "idle.hpp"

namespace qilin {

// Registers as a set: what a function and everything it calls may load from the
// Cpu or write back to it. A caller writes back before a call, and reads again
// after it, only its registers in the callee's set: the rest stay in its locals,
// where nothing the callee reaches can see or change them.
struct RegSet {
    uint32_t gpr = 0, fpr0 = 0, fpr1 = 0;  // bit n: rn, ps0 of FPR n, ps1 of FPR n
    uint32_t other = 0;                    // bits 0-7: CR fields 0-7; 8 LR, 9 CTR, 10-12 XER's SO, OV, CA
    bool all = false;                      // everything: it calls through a pointer, or isn't lifted code
    void add(const RegSet& o) { gpr |= o.gpr, fpr0 |= o.fpr0, fpr1 |= o.fpr1, other |= o.other, all |= o.all; }
    bool operator==(const RegSet&) const = default;
};
// One function's own registers, and the functions it calls, tail-calls or falls into.
struct CallSummary {
    RegSet own;
    std::set<uint32_t> callees;
};

struct LiftInput {
    uint32_t start = 0;
    std::vector<std::pair<uint32_t, ppc::Insn>> insns;  // reachable instructions, by address
    std::vector<JumpTable> jtabs;
    std::set<uint32_t> not_calls;                       // `bl` to the next instruction (reads the PC)
    uint32_t falls_into = 0;                            // the next function, if execution runs into it
    std::string name;                                   // its C++ name, if not fn_<start> (a wrapped function's body)
    bool single_leaf_only = false;                      // single mode for leaf functions only, as before v0.23 (to measure against)
    std::function<bool(uint32_t)> is_function;          // a known function start (tail and call targets)
    std::function<bool(uint32_t)> outside_image;        // not in the DOL's image: a `bla` there goes to the dispatcher
    // What a called function, with all it calls, may touch (close_over_calls); null, or a null
    // answer: everything, and every register goes back and forth around the call.
    std::function<const RegSet*(uint32_t)> callee_regs;
    // A call target that is pure (idle.hpp), with its effects: lets poll loops call getters.
    std::function<bool(uint32_t, RegEffects&)> pure_call;
};

struct Lifted {
    bool ok = false;
    std::string code;   // the definition, when ok
    std::string why;    // why not, when !ok: the first instruction outside the slice
    uint32_t why_at = 0;
};

std::string fn_name(uint32_t address);  // "fn_80004000"

// Is this instruction in the current slice?
bool in_slice(const ppc::Insn& in, std::string* why = nullptr);

Lifted lift(const LiftInput& f);

// For LiftInput::callee_regs: each function's summary, then the sets closed over the call
// graph (a callee with no summary makes its callers' sets "all").
CallSummary summarize(const LiftInput& f);
std::map<uint32_t, RegSet> close_over_calls(const std::map<uint32_t, CallSummary>& summaries);

// The input for one of Cfg's functions.
LiftInput lift_input(const Cfg& cfg, const Function& f);

}  // namespace qilin
