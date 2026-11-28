// qilin-dsp: a static recompiler for GameCube DSP microcode.
//
//   qilin-dsp lift <main.dol> -o <out.cpp>          the microcodes this knows, from the player's DOL
//   qilin-dsp lift --words <file> [--roots a,b,..] -o <out.cpp>
//                                                    a program as hexadecimal words in text (tests)
//
// The output is one C++ function per microcode, written against
// mkdd/src/dsp_core.hpp: every instruction becomes a statement or two on the
// DSP's state, jumps become gotos, and the function returns when the DSP
// halts, jumps into its ROM or can only wait for the CPU (dsp_core.hpp says
// how that is known). Status flags nobody tests are not computed. It is generated into the build tree and never kept: it
// is the game's code in another form.
//
// What is lifted is what is reached from the vectors (0x0000-0x0010, two
// words each) and from the roots given for the microcode: targets of
// computed jumps cannot be found by following the code. An address reached
// at run time that was not lifted stops the game with its number.
//
// The instruction encodings are the publicly documented ones (the "GameCube
// DSP User's Manual" kept with Dolphin); the table is this file's own.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "command_line.hpp"
#include "dol.hpp"

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "qilin-dsp: %s\n", message.c_str());
    std::exit(1);
}
std::string hex4(unsigned v) {
    char b[8];
    std::snprintf(b, sizeof b, "%04X", v & 0xFFFF);
    return b;
}
std::string num(long long v) { return std::to_string(v); }

// ---- decoding ----

struct Pattern {
    const char* bits;  // first word (and, after a space, the second); letters name fields, x is any
    const char* name;
};
// Instructions that cannot carry an extension.
constexpr Pattern kMain[] = {
    {"0000000000000000", "nop"},
    {"00000000000001dd", "dar"},
    {"00000000000010dd", "iar"},
    {"00000000000011dd", "subarn"},
    {"000000000001ssdd", "addarn"},
    {"0000000000100001", "halt"},
    {"00000000010rrrrr", "loop"},
    {"00000000011rrrrr aaaaaaaaaaaaaaaa", "bloop"},
    {"00000000100ddddd iiiiiiiiiiiiiiii", "lri"},
    {"00000000110ddddd mmmmmmmmmmmmmmmm", "lr"},
    {"00000000111sssss mmmmmmmmmmmmmmmm", "sr"},
    {"000000100111cccc", "if"},
    {"000000101001cccc aaaaaaaaaaaaaaaa", "j"},
    {"000000101011cccc aaaaaaaaaaaaaaaa", "call"},
    {"000000101101cccc", "ret"},
    {"000000101111cccc", "rti"},
    {"0000001d00000000 iiiiiiiiiiiiiiii", "addi"},
    {"0000001d00100000 iiiiiiiiiiiiiiii", "xori"},
    {"0000001d01000000 iiiiiiiiiiiiiiii", "andi"},
    {"0000001d01100000 iiiiiiiiiiiiiiii", "ori"},
    {"0000001d10000000 iiiiiiiiiiiiiiii", "cmpi"},
    {"0000001d10100000 iiiiiiiiiiiiiiii", "andf"},
    {"0000001d11000000 iiiiiiiiiiiiiiii", "andcf"},
    {"0000001d000100ss", "ilrr"},
    {"0000001d000101ss", "ilrrd"},
    {"0000001d000110ss", "ilrri"},
    {"0000001d000111ss", "ilrrn"},
    {"0000001011001010", "lsrn"},
    {"0000001011001011", "asrn"},
    {"0000010diiiiiiii", "addis"},
    {"0000011diiiiiiii", "cmpis"},
    {"00001dddiiiiiiii", "lris"},
    {"00010000iiiiiiii", "loopi"},
    {"00010001iiiiiiii aaaaaaaaaaaaaaaa", "bloopi"},
    {"0001001000000iii", "sbclr"},
    {"0001001100000iii", "sbset"},
    {"0001010r00iiiiii", "lsl"},
    {"0001010r01iiiiii", "lsr"},
    {"0001010r10iiiiii", "asl"},
    {"0001010r11iiiiii", "asr"},
    {"00010110mmmmmmmm iiiiiiiiiiiiiiii", "si"},
    {"00010111rrr0cccc", "jr"},
    {"00010111rrr1cccc", "callr"},
    {"000110000ssddddd", "lrr"},
    {"000110001ssddddd", "lrrd"},
    {"000110010ssddddd", "lrri"},
    {"000110011ssddddd", "lrrn"},
    {"000110100ddsssss", "srr"},
    {"000110101ddsssss", "srrd"},
    {"000110110ddsssss", "srri"},
    {"000110111ddsssss", "srrn"},
    {"000111dddddsssss", "mrr"},
    {"00100dddmmmmmmmm", "lrs"},
    {"0010100smmmmmmmm", "srsh"},
    {"001011ssmmmmmmmm", "srs"},
    // From here on the low bits are an extension (seven of them in 0x3000-0x3FFF).
    {"001100sd0xxxxxxx", "xorr"},
    {"001101sd0xxxxxxx", "andr"},
    {"001110sd0xxxxxxx", "orr"},
    {"0011110d0xxxxxxx", "andc"},
    {"0011111d0xxxxxxx", "orc"},
    {"0011000d1xxxxxxx", "xorc"},
    {"0011001d1xxxxxxx", "not"},
    {"001101sd1xxxxxxx", "lsrnrx"},
    {"001110sd1xxxxxxx", "asrnrx"},
    {"0011110d1xxxxxxx", "lsrnr"},
    {"0011111d1xxxxxxx", "asrnr"},
    {"01000ssdxxxxxxxx", "addr"},
    {"010010sdxxxxxxxx", "addax"},
    {"0100110dxxxxxxxx", "add"},
    {"0100111dxxxxxxxx", "addp"},
    {"01010ssdxxxxxxxx", "subr"},
    {"010110sdxxxxxxxx", "subax"},
    {"0101110dxxxxxxxx", "sub"},
    {"0101111dxxxxxxxx", "subp"},
    {"01100ssdxxxxxxxx", "movr"},
    {"011010sdxxxxxxxx", "movax"},
    {"0110110dxxxxxxxx", "mov"},
    {"0110111dxxxxxxxx", "movp"},
    {"011100sdxxxxxxxx", "addaxl"},
    {"0111010dxxxxxxxx", "incm"},
    {"0111011dxxxxxxxx", "inc"},
    {"0111100dxxxxxxxx", "decm"},
    {"0111101dxxxxxxxx", "dec"},
    {"0111110dxxxxxxxx", "neg"},
    {"0111111dxxxxxxxx", "movnp"},
    {"1000r000xxxxxxxx", "nx"},
    {"1000d001xxxxxxxx", "clr"},
    {"10000010xxxxxxxx", "cmp"},
    {"10000011xxxxxxxx", "mulaxh"},
    {"10000100xxxxxxxx", "clrp"},
    {"10000101xxxxxxxx", "tstprod"},
    {"1000011rxxxxxxxx", "tstaxh"},
    {"10001010xxxxxxxx", "m2"},
    {"10001011xxxxxxxx", "m0"},
    {"10001100xxxxxxxx", "clr15"},
    {"10001101xxxxxxxx", "set15"},
    {"10001110xxxxxxxx", "set16"},
    {"10001111xxxxxxxx", "set40"},
    {"1001s000xxxxxxxx", "mul"},
    {"1001d001xxxxxxxx", "asr16"},
    {"1001s01dxxxxxxxx", "mulmvz"},
    {"1001s10dxxxxxxxx", "mulac"},
    {"1001s11dxxxxxxxx", "mulmv"},
    {"101st000xxxxxxxx", "mulx"},
    {"1010d001xxxxxxxx", "abs"},
    {"1011d001xxxxxxxx", "tst"},
    {"101st01dxxxxxxxx", "mulxmvz"},
    {"101st10dxxxxxxxx", "mulxac"},
    {"101st11dxxxxxxxx", "mulxmv"},
    {"110st000xxxxxxxx", "mulc"},
    {"110rs001xxxxxxxx", "cmpaxh"},
    {"110st01dxxxxxxxx", "mulcmvz"},
    {"110st10dxxxxxxxx", "mulcac"},
    {"110st11dxxxxxxxx", "mulcmv"},
    {"111000stxxxxxxxx", "maddx"},
    {"111001stxxxxxxxx", "msubx"},
    {"111010stxxxxxxxx", "maddc"},
    {"111011stxxxxxxxx", "msubc"},
    {"1111000dxxxxxxxx", "lsl16"},
    {"1111001sxxxxxxxx", "madd"},
    {"1111010dxxxxxxxx", "lsr16"},
    {"1111011sxxxxxxxx", "msub"},
    {"111110sdxxxxxxxx", "addpaxz"},
    {"1111110dxxxxxxxx", "clrl"},
    {"1111111dxxxxxxxx", "movpz"},
};
constexpr Pattern kExt[] = {
    {"000000xx", ""},     {"000001rr", "dr"},   {"000010rr", "ir"},    {"000011rr", "nr"},    {"0001ddss", "mv"},
    {"001ss0dd", "s"},    {"001ss1dd", "sn"},   {"01ddd0ss", "l"},     {"01ddd1ss", "ln"},    {"10dd000s", "ls"},
    {"10dd010s", "lsn"},  {"10dd100s", "lsm"},  {"10dd110s", "lsnm"},  {"10dd001s", "sl"},    {"10dd011s", "sln"},
    {"10dd101s", "slm"},  {"10dd111s", "slnm"}, {"11sr0011", "ldax"},  {"11sr0111", "ldaxn"}, {"11sr1011", "ldaxm"},
    {"11sr1111", "ldaxnm"}, {"11dr00ss", "ld"}, {"11dr01ss", "ldn"},   {"11dr10ss", "ldm"},   {"11dr11ss", "ldnm"},
};

struct Fields {
    int v[26];
    bool has[26];
    int operator[](char c) const { return v[c - 'a']; }
};
bool match(const Pattern& p, uint32_t raw, unsigned total_bits, Fields& f) {
    f = Fields{};
    unsigned bit = total_bits;
    for (const char* c = p.bits; *c; ++c) {
        if (*c == ' ') continue;
        --bit;
        const unsigned b = (raw >> bit) & 1;
        if (*c == '0' || *c == '1') {
            if (b != unsigned(*c - '0')) return false;
        } else if (*c != 'x') {
            f.v[*c - 'a'] = (f.v[*c - 'a'] << 1) | static_cast<int>(b);
            f.has[*c - 'a'] = true;
        }
    }
    return true;
}
unsigned words_of(const Pattern& p) { return std::string_view(p.bits).find(' ') == std::string_view::npos ? 1 : 2; }

struct Insn {
    uint16_t addr = 0;
    unsigned size = 1;
    bool valid = false;
    std::string name, ext;
    Fields f{}, e{};
    uint16_t word = 0;
};

Insn decode(const std::vector<uint16_t>& w, uint16_t at) {
    Insn in;
    in.addr = at;
    in.word = w[at];
    for (const Pattern& p : kMain) {
        const unsigned n = words_of(p);
        Fields f;
        if (n == 1) {
            if (!match(p, w[at], 16, f)) continue;
        } else {
            // Match on the first word alone; the second is all fields.
            const uint32_t raw = (uint32_t(w[at]) << 16) | (at + 1u < w.size() ? w[at + 1] : 0);
            if (!match(p, raw, 32, f)) continue;
            if (at + 1u >= w.size()) return in;
        }
        in.size = n;
        in.name = p.name;
        in.f = f;
        in.valid = true;
        break;
    }
    if (!in.valid) return in;
    if ((in.word >> 12) >= 3) {
        const unsigned e = in.word & ((in.word >> 12) == 3 ? 0x7F : 0xFF);
        bool found = false;
        for (const Pattern& p : kExt) {
            Fields f;
            if (!match(p, e, 8, f)) continue;
            in.ext = p.name;
            in.e = f;
            found = true;
            break;
        }
        if (!found) in.valid = false;
    }
    return in;
}

bool is(const Insn& in, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (in.name == n) return true;
    return false;
}
bool conditional(const Insn& in) { return in.f['c'] != 15; }

// ---- emitting ----
//
// The generated function keeps the registers in a local `r` (dsp_core.hpp says why) and writes
// them back to the core `s` wherever it leaves.

// Conditions on the status register, by code.
const char* kCond[16] = {
    "!(((r.sr >> 1) ^ (r.sr >> 3)) & 1)",                     // GE: O == S
    "(((r.sr >> 1) ^ (r.sr >> 3)) & 1)",                      // L
    "(!(((r.sr >> 1) ^ (r.sr >> 3)) & 1) && !(r.sr & 4))",    // G
    "((((r.sr >> 1) ^ (r.sr >> 3)) & 1) || (r.sr & 4))",      // LE
    "!(r.sr & 4)",                                            // NZ
    "(r.sr & 4)",                                             // Z
    "!(r.sr & 1)",                                            // NC
    "(r.sr & 1)",                                             // C
    "!(r.sr & 0x10)",                                         // below s32
    "(r.sr & 0x10)",                                          // above s32
    "((r.sr & 0x30) && !(r.sr & 4))",
    "(!(r.sr & 0x30) || (r.sr & 4))",
    "!(r.sr & 0x40)",                                         // LNZ
    "(r.sr & 0x40)",                                          // LZ
    "(r.sr & 2)",                                             // O
    "true",
};
// The flags each condition reads.
constexpr uint8_t kCondUses[16] = {0x0A, 0x0A, 0x0E, 0x0E, 0x04, 0x04, 0x01, 0x01, 0x10, 0x10, 0x34, 0x34, 0x40, 0x40, 0x02, 0x00};
std::string cond(int cc) { return kCond[cc]; }

struct Lifter {
    std::vector<uint16_t> words;
    std::map<uint16_t, Insn> code;        // reached instructions
    std::set<uint16_t> leaders;           // addresses the dispatcher knows
    std::set<uint16_t> looped;            // instructions repeated by a LOOP before them
    std::map<uint16_t, std::vector<uint16_t>> loop_ends;  // last word of a block loop's body -> its starts
    std::set<uint16_t> tails;             // instructions whose tail label is jumped to
    std::map<uint16_t, uint8_t> live_in, live_out;        // status flags (bits 0-7) wanted before and after
    std::string where;                    // for messages
    unsigned quiet = 0, flagged = 0;      // flag-setting instructions lifted without and with their flags

    [[noreturn]] void bad(const Insn& in, const std::string& what) const {
        fail(where + ": " + hex4(in.addr) + " (" + in.name + (in.ext.empty() ? "" : "'" + in.ext) + "): " + what);
    }

    static std::string ac(int i) { return "r.ac" + num(i); }
    static std::string ar(int i) { return "r.ar" + num(i); }
    static std::string ix(int i) { return "r.ix" + num(i); }
    static std::string wr(int i) { return "r.wr" + num(i); }
    static std::string axl(int i) { return "r.axl" + num(i); }
    static std::string axh(int i) { return "r.axh" + num(i); }
    static std::string inc(int i) { return "ar_inc(" + ar(i) + ", " + wr(i) + ")"; }
    static std::string dec(int i) { return "ar_dec(" + ar(i) + ", " + wr(i) + ")"; }
    static std::string add_ix(int i, int j) { return "ar_add(" + ar(i) + ", " + wr(i) + ", " + ix(j) + ")"; }

    // Registers by number, as the generic moves and loads see them.
    std::string rread(const Insn& in, int n, bool saturate) const {
        if (n < 4) return ar(n);
        if (n < 8) return ix(n - 4);
        if (n < 12) return wr(n - 8);
        switch (n) {
            case 0x10: case 0x11: return "ach(" + ac(n - 0x10) + ")";
            case 0x12: return "r.config";
            case 0x13: return "r.sr";
            case 0x18: case 0x19: return axl(n - 0x18);
            case 0x1A: case 0x1B: return axh(n - 0x1A);
            case 0x1C: case 0x1D: return "acl(" + ac(n - 0x1C) + ")";
            case 0x1E: case 0x1F: return (saturate ? "acm_sat(r.sr, " : "acm(") + ac(n - 0x1E) + ")";
            default: bad(in, "reads register " + num(n) + " (a stack or a part of the product): not handled");
        }
    }
    std::string rwrite(const Insn& in, int n, const std::string& v) const {
        const std::string u = "static_cast<uint16_t>(" + v + ")";
        if (n < 4) return ar(n) + " = " + u + ";";
        if (n < 8) return ix(n - 4) + " = " + u + ";";
        if (n < 12) return wr(n - 8) + " = " + u + ";";
        switch (n) {
            case 0x10: case 0x11: return ac(n - 0x10) + " = set_ach(" + ac(n - 0x10) + ", " + u + ");";
            case 0x12: return "r.config = " + u + " & 0xFF;";
            case 0x13: return "r.sr = " + u + " & ~0x0100;";
            case 0x18: case 0x19: return axl(n - 0x18) + " = " + u + ";";
            case 0x1A: case 0x1B: return axh(n - 0x1A) + " = " + u + ";";
            case 0x1C: case 0x1D: return ac(n - 0x1C) + " = set_acl(" + ac(n - 0x1C) + ", " + u + ");";
            case 0x1E: case 0x1F: return ac(n - 0x1E) + " = set_acm(r.sr, " + ac(n - 0x1E) + ", " + u + ");";
            default: bad(in, "writes register " + num(n) + " (a stack or a part of the product): not handled");
        }
    }

    // The accumulator an arithmetic instruction writes, or -1.
    static int main_dest(const Insn& in) {
        static const char* writers[] = {"xorr", "andr", "orr", "andc", "orc", "xorc", "not", "lsrnrx", "asrnrx", "lsrnr", "asrnr",
            "addr", "addax", "add", "addp", "subr", "subax", "sub", "subp", "movr", "movax", "mov", "movp", "addaxl", "incm",
            "inc", "decm", "dec", "neg", "movnp", "clr", "asr16", "mulmvz", "mulac", "mulmv", "abs", "mulxmvz", "mulxac",
            "mulxmv", "mulcmvz", "mulcac", "mulcmv", "lsl16", "lsr16", "addpaxz", "clrl", "movpz"};
        for (const char* w : writers)
            if (in.name == w) return in.f['d'];
        return -1;
    }

    // The status flags an instruction sets (all of them it sets, it sets whatever they were).
    static uint8_t flag_defs(const Insn& in) {
        static const char* arithmetic[] = {"addi", "xori", "andi", "ori", "cmpi", "addis", "cmpis", "lsl", "lsr", "asl", "asr", "lsrn",
            "asrn", "cmp", "tstprod", "tstaxh", "tst", "cmpaxh"};
        if (main_dest(in) >= 0) return 0x3F;
        for (const char* a : arithmetic)
            if (in.name == a) return 0x3F;
        if (in.name == "andf" || in.name == "andcf") return 0x40;
        if (is(in, {"lri", "lr", "lrr", "lrrd", "lrri", "lrrn", "mrr"}) && in.f['d'] == 0x13) return 0xFF;
        return 0;
    }
    // The status flags an instruction reads.
    static uint8_t flag_uses(const Insn& in) {
        if (is(in, {"if", "j", "call", "ret", "rti", "jr", "callr"})) return kCondUses[in.f['c']];
        if (is(in, {"mrr", "sr", "srr", "srrd", "srri", "srrn"}) && in.f['s'] == 0x13) return 0xFF;
        if (is(in, {"loop", "bloop"}) && in.f['r'] == 0x13) return 0xFF;
        return 0;
    }

    // The extension: what it does before the instruction (loads, stores, new addresses, all from
    // the state as it was) and what it writes after.
    void extension(const Insn& in, std::string& pre, std::string& post) const {
        const std::string& x = in.ext;
        if (x.empty()) return;
        const Fields& e = in.e;
        auto set_ar = [&](int i, const std::string& name, bool by_index) {
            pre += "const uint16_t " + name + " = " + (by_index ? add_ix(i, i) : inc(i)) + "; ";
            post += ar(i) + " = " + name + "; ";
        };
        if (x == "dr") {
            pre += "const uint16_t a_ = " + dec(e['r']) + "; ";
            post += ar(e['r']) + " = a_; ";
        } else if (x == "ir") {
            set_ar(e['r'], "a_", false);
        } else if (x == "nr") {
            set_ar(e['r'], "a_", true);
        } else if (x == "mv") {
            pre += "const uint16_t v_ = " + rread(in, 0x1C + e['s'], true) + "; ";
            post += rwrite(in, 0x18 + e['d'], "v_") + " ";
        } else if (x == "s" || x == "sn") {
            pre += "s.wr(" + ar(e['d']) + ", " + rread(in, 0x1C + e['s'], true) + "); ";
            set_ar(e['d'], "a_", x == "sn");
        } else if (x == "l" || x == "ln") {
            const int dest = 0x18 + e['d'];
            if (dest >= 0x1C && main_dest(in) == (dest & 1))
                bad(in, "the extension loads into the accumulator the instruction writes: not handled");
            pre += "const uint16_t v_ = s.rd(" + ar(e['s']) + "); ";
            set_ar(e['s'], "a_", x == "ln");
            post += rwrite(in, dest, "v_") + " ";
        } else if (x == "ls" || x == "lsn" || x == "lsm" || x == "lsnm") {
            pre += "s.wr(" + ar(3) + ", acm_sat(r.sr, " + ac(e['s']) + ")); const uint16_t v_ = s.rd(" + ar(0) + "); ";
            set_ar(3, "a3_", x == "lsm" || x == "lsnm");
            set_ar(0, "a0_", x == "lsn" || x == "lsnm");
            post += rwrite(in, 0x18 + e['d'], "v_") + " ";
        } else if (x == "sl" || x == "sln" || x == "slm" || x == "slnm") {
            pre += "s.wr(" + ar(0) + ", acm_sat(r.sr, " + ac(e['s']) + ")); const uint16_t v_ = s.rd(" + ar(3) + "); ";
            set_ar(3, "a3_", x == "slm" || x == "slnm");
            set_ar(0, "a0_", x == "sln" || x == "slnm");
            post += rwrite(in, 0x18 + e['d'], "v_") + " ";
        } else if (x == "ld" || x == "ldn" || x == "ldm" || x == "ldnm") {
            const int src = e['s'];  // 0-2 (3 is LDAX)
            pre += "const uint16_t v_ = s.rd(" + ar(src) + "); const uint16_t w_ = s.rd_second(" + ar(src) + ", " + ar(3) + "); ";
            set_ar(src, "a_", x == "ldn" || x == "ldnm");
            set_ar(3, "a3_", x == "ldm" || x == "ldnm");
            post += rwrite(in, 0x18 + e['d'] * 2, "v_") + " " + rwrite(in, 0x19 + e['r'] * 2, "w_") + " ";
        } else if (x == "ldax" || x == "ldaxn" || x == "ldaxm" || x == "ldaxnm") {
            const int src = e['s'];  // ar0 or ar1
            pre += "const uint16_t v_ = s.rd(" + ar(src) + "); const uint16_t w_ = s.rd_second(" + ar(src) + ", " + ar(3) + "); ";
            set_ar(src, "a_", x == "ldaxn" || x == "ldaxnm");
            set_ar(3, "a3_", x == "ldaxm" || x == "ldaxnm");
            post += axh(e['r']) + " = v_; " + axl(e['r']) + " = w_; ";
        } else {
            bad(in, "extension not handled");
        }
    }

    // An instruction that is not a transfer of control, as statements. `live`: the flags wanted after it.
    std::string plain(const Insn& in, uint8_t live) {
        const Fields& f = in.f;
        const std::string& n = in.name;
        const uint8_t defs = flag_defs(in);
        const bool flags = (defs & live) != 0;
        if (defs && defs != 0xFF) ++(flags ? flagged : quiet);
        const std::string F = flags ? "r.sr, " : "";  // the flag-setting form of a function, or the plain one
        const std::string d = num(f['d']), sx = num(f['s']), t = num(f['t']);
        const std::string A = ac(f['d']);
        const std::string other = ac(1 - f['d']);
        auto s16 = [](const std::string& v) { return "(int64_t(static_cast<int16_t>(" + v + ")) << 16)"; };
        const std::string imm = num(f['i']);
        const std::string imm16 = "(int64_t(" + num(static_cast<int16_t>(f['i'])) + ") << 16)";
        const std::string imm8 = "(int64_t(" + num(static_cast<int8_t>(f['i'])) + ") << 16)";
        const std::string short_address = "static_cast<uint16_t>((r.config << 8) | " + num(f['m']) + ")";
        auto u16 = [](const std::string& v) { return "static_cast<uint16_t>(" + v + ")"; };
        auto compare = [&](const std::string& a, const std::string& b) { return flags ? "(void)sub(r.sr, " + a + ", " + b + ");" : std::string(); };
        std::string m;  // the instruction itself

        if (n == "nop" || n == "nx") m = "";
        else if (n == "dar") m = ar(f['d']) + " = " + dec(f['d']) + ";";
        else if (n == "iar") m = ar(f['d']) + " = " + inc(f['d']) + ";";
        else if (n == "subarn") m = ar(f['d']) + " = ar_sub(" + ar(f['d']) + ", " + wr(f['d']) + ", " + ix(f['d']) + ");";
        else if (n == "addarn") m = ar(f['d']) + " = " + add_ix(f['d'], f['s']) + ";";
        else if (n == "lri") m = rwrite(in, f['d'], imm);
        else if (n == "lr") m = rwrite(in, f['d'], "s.rd(" + num(f['m']) + ")");
        else if (n == "sr") m = "s.wr(" + num(f['m']) + ", " + rread(in, f['s'], true) + ");";
        else if (n == "addi") m = A + " = add(" + F + A + ", " + imm16 + ");";
        else if (n == "xori") m = A + " = logic(" + F + A + ", " + u16("acm(" + A + ") ^ " + imm) + ");";
        else if (n == "andi") m = A + " = logic(" + F + A + ", " + u16("acm(" + A + ") & " + imm) + ");";
        else if (n == "ori") m = A + " = logic(" + F + A + ", " + u16("acm(" + A + ") | " + imm) + ");";
        else if (n == "cmpi") m = compare(A, imm16);
        else if (n == "andf") m = flags ? "r.sr = static_cast<uint16_t>((acm(" + A + ") & " + imm + ") == 0 ? (r.sr | 0x40) : (r.sr & ~0x40));" : "";
        else if (n == "andcf")
            m = flags ? "r.sr = static_cast<uint16_t>((acm(" + A + ") & " + imm + ") == " + imm + " ? (r.sr | 0x40) : (r.sr & ~0x40));" : "";
        else if (n == "ilrr" || n == "ilrrd" || n == "ilrri" || n == "ilrrn") {
            const int a = f['s'];
            const std::string step = n == "ilrrd" ? ar(a) + " = " + dec(a) + "; " : n == "ilrri" ? ar(a) + " = " + inc(a) + "; "
                                   : n == "ilrrn" ? ar(a) + " = " + add_ix(a, a) + "; " : "";
            m = "{ const uint16_t i_ = s.irom(" + ar(a) + "); " + A + " = set_acm(r.sr, " + A + ", i_); " + step + "}";
        } else if (n == "lsrn") m = "r.ac0 = lsrn_ac(" + F + "r.ac0, acm(r.ac1));";
        else if (n == "asrn") m = "r.ac0 = asrn_ac(" + F + "r.ac0, acm(r.ac1));";
        else if (n == "addis") m = A + " = add(" + F + A + ", " + imm8 + ");";
        else if (n == "cmpis") m = compare(A, imm8);
        else if (n == "lris") m = rwrite(in, 0x18 + f['d'], num(static_cast<uint16_t>(static_cast<int8_t>(f['i']))));
        else if (n == "sbclr") m = "r.sr &= ~" + num(1ll << (f['i'] + 6)) + ";";
        else if (n == "sbset") m = "r.sr |= " + num(1ll << (f['i'] + 6)) + ";";
        else if (n == "lsl" || n == "asl") m = ac(f['r']) + " = lsl(" + F + ac(f['r']) + ", " + imm + ");";
        else if (n == "lsr" || n == "asr") m = ac(f['r']) + " = " + n + "(" + F + ac(f['r']) + ", " + num(f['i'] ? 64 - f['i'] : 0) + ");";
        else if (n == "si") m = "s.hw_write(" + num(0xFF00 | f['m']) + ", " + imm + ");";
        else if (n == "lrr" || n == "lrrd" || n == "lrri" || n == "lrrn") {
            const int a = f['s'];
            const std::string step = n == "lrrd" ? ar(a) + " = " + dec(a) + "; " : n == "lrri" ? ar(a) + " = " + inc(a) + "; "
                                   : n == "lrrn" ? ar(a) + " = " + add_ix(a, a) + "; " : "";
            m = "{ const uint16_t v_ = s.rd(" + ar(a) + "); " + rwrite(in, f['d'], "v_") + " " + step + "}";
        } else if (n == "srr" || n == "srrd" || n == "srri" || n == "srrn") {
            const int a = f['d'];
            const std::string step = n == "srrd" ? ar(a) + " = " + dec(a) + ";" : n == "srri" ? ar(a) + " = " + inc(a) + ";"
                                   : n == "srrn" ? ar(a) + " = " + add_ix(a, a) + ";" : "";
            m = "s.wr(" + ar(a) + ", " + rread(in, f['s'], true) + "); " + step;
        } else if (n == "mrr") m = "{ const uint16_t v_ = " + rread(in, f['s'], true) + "; " + rwrite(in, f['d'], "v_") + " }";
        else if (n == "lrs") m = rwrite(in, 0x18 + f['d'], "s.rd(" + short_address + ")");
        else if (n == "srsh") m = "s.wr(" + short_address + ", ach(" + ac(f['s']) + "));";
        else if (n == "srs") m = "s.wr(" + short_address + ", " + rread(in, 0x1C + f['s'], true) + ");";
        else if (n == "xorr") m = A + " = logic(" + F + A + ", " + u16("acm(" + A + ") ^ " + axh(f['s'])) + ");";
        else if (n == "andr") m = A + " = logic(" + F + A + ", " + u16("acm(" + A + ") & " + axh(f['s'])) + ");";
        else if (n == "orr") m = A + " = logic(" + F + A + ", " + u16("acm(" + A + ") | " + axh(f['s'])) + ");";
        else if (n == "andc") m = A + " = logic(" + F + A + ", " + u16("acm(" + A + ") & acm(" + other + ")") + ");";
        else if (n == "orc") m = A + " = logic(" + F + A + ", " + u16("acm(" + A + ") | acm(" + other + ")") + ");";
        else if (n == "xorc") m = A + " = logic(" + F + A + ", " + u16("acm(" + A + ") ^ acm(" + other + ")") + ");";
        else if (n == "not") m = A + " = logic(" + F + A + ", " + u16("~acm(" + A + ")") + ");";
        else if (n == "lsrnrx") m = A + " = lsrn(" + F + A + ", " + axh(f['s']) + ");";
        else if (n == "asrnrx") m = A + " = asrn(" + F + A + ", " + axh(f['s']) + ");";
        else if (n == "lsrnr") m = A + " = lsrn(" + F + A + ", acm(" + other + "));";
        else if (n == "asrnr") m = A + " = asrn(" + F + A + ", acm(" + other + "));";
        else if (n == "addr") m = A + " = add(" + F + A + ", " + s16(rread(in, 0x18 + f['s'], false)) + ");";
        else if (n == "addax") m = A + " = add(" + F + A + ", ax(" + axh(f['s']) + ", " + axl(f['s']) + "));";
        else if (n == "add") m = A + " = add(" + F + A + ", " + other + ");";
        else if (n == "addp") m = A + " = add(" + F + A + ", r.prod);";
        else if (n == "subr") m = A + " = sub(" + F + A + ", " + s16(rread(in, 0x18 + f['s'], false)) + ");";
        else if (n == "subax") m = A + " = sub(" + F + A + ", ax(" + axh(f['s']) + ", " + axl(f['s']) + "));";
        else if (n == "sub") m = A + " = sub(" + F + A + ", " + other + ");";
        else if (n == "subp") m = A + " = sub(" + F + A + ", r.prod);";
        else if (n == "movr") m = A + " = mov(" + F + s16(rread(in, 0x18 + f['s'], false)) + ");";
        else if (n == "movax") m = A + " = mov(" + F + "ax(" + axh(f['s']) + ", " + axl(f['s']) + "));";
        else if (n == "mov") m = A + " = mov(" + F + other + ");";
        else if (n == "movp") m = A + " = mov(" + F + "r.prod);";
        else if (n == "movnp") m = A + " = mov(" + F + "-r.prod);";
        else if (n == "movpz") m = A + " = mov(" + F + "round_low(r.prod));";
        else if (n == "addaxl") m = A + " = add(" + F + A + ", int64_t(" + axl(f['s']) + "));";
        else if (n == "incm") m = A + " = add(" + F + A + ", 0x10000);";
        else if (n == "inc") m = A + " = add(" + F + A + ", 1);";
        else if (n == "decm") m = A + " = sub(" + F + A + ", 0x10000);";
        else if (n == "dec") m = A + " = sub(" + F + A + ", 1);";
        else if (n == "neg") m = A + " = neg(" + F + A + ");";
        else if (n == "abs") m = "if (" + A + " < 0) " + A + " = sx40(-" + A + "); tst(" + F + A + ");";
        else if (n == "clr") m = A + " = mov(" + F + "0);";
        else if (n == "tst") m = flags ? "tst(r.sr, " + A + ");" : "";
        else if (n == "cmp") m = compare("r.ac0", "r.ac1");
        else if (n == "cmpaxh") m = compare(ac(f['s']), s16(axh(f['r'])));
        else if (n == "clrp") m = "r.prod = 0;";
        else if (n == "tstprod") m = flags ? "tst(r.sr, r.prod);" : "";
        else if (n == "tstaxh") m = flags ? "flags16(r.sr, " + axh(f['r']) + ", false);" : "";
        else if (n == "m2") m = "r.sr &= ~0x2000;";
        else if (n == "m0") m = "r.sr |= 0x2000;";
        else if (n == "clr15") m = "r.sr &= ~0x8000;";
        else if (n == "set15") m = "r.sr |= 0x8000;";
        else if (n == "set16") m = "r.sr &= ~0x4000;";
        else if (n == "set40") m = "r.sr |= 0x4000;";
        else if (n == "asr16") m = A + " = asr(" + F + A + ", 16);";
        else if (n == "lsl16") m = A + " = lsl(" + F + A + ", 16);";
        else if (n == "lsr16") m = A + " = lsr(" + F + A + ", 16);";
        else if (n == "clrl") m = A + " = mov(" + F + "round_low(" + A + "));";
        else if (n == "addpaxz") {
            const std::string sum = "sx40(round_low(r.prod) + " + s16(axh(f['s'])) + ")";
            if (flags) m = "{ const int64_t r_ = " + sum + "; flags64(r.sr, r_, static_cast<uint64_t>(r.prod) > static_cast<uint64_t>(r_)); " + A + " = r_; }";
            else m = A + " = " + sum + ";";
        } else {
            // The multiplier's family: what is multiplied, and what happens to the old product.
            std::string product;
            if (n.starts_with("mulx") || n == "maddx" || n == "msubx") {
                const std::string a = f['s'] ? "r.axh0" : "r.axl0", b = f['t'] ? "r.axh1" : "r.axl1";
                if (!f['s'] && !f['t']) product = "mul(r.sr, " + a + ", " + b + ", 1)";
                else if (!f['s']) product = "mul(r.sr, " + a + ", " + b + ", 2)";
                else if (!f['t']) product = "mul(r.sr, " + b + ", " + a + ", 2)";
                else product = "mul(r.sr, " + a + ", " + b + ")";
            } else if (n.starts_with("mulc") || n == "maddc" || n == "msubc") product = "mul(r.sr, acm(" + ac(f['s']) + "), " + axh(f['t']) + ")";
            else if (n == "mulaxh") product = "mul(r.sr, r.axh0, r.axh0)";
            else if (n.starts_with("mul") || n == "madd" || n == "msub") product = "mul(r.sr, " + axl(f['s']) + ", " + axh(f['s']) + ")";
            else bad(in, "instruction not handled");
            if (n.starts_with("madd")) m = "r.prod = sx40(r.prod + " + product + ");";
            else if (n.starts_with("msub")) m = "r.prod = sx40(r.prod - " + product + ");";
            else if (n.ends_with("mvz")) m = "{ const int64_t p_ = round_low(r.prod); r.prod = sx40(" + product + "); " + A + " = mov(" + F + "p_); }";
            else if (n.ends_with("mv")) m = "{ const int64_t p_ = r.prod; r.prod = sx40(" + product + "); " + A + " = mov(" + F + "p_); }";
            else if (n.ends_with("ac")) m = "{ const int64_t p_ = " + A + " + r.prod; r.prod = sx40(" + product + "); " + A + " = mov(" + F + "p_); }";
            else m = "r.prod = sx40(" + product + ");";
        }
        std::string pre, post;
        extension(in, pre, post);
        if (pre.empty() && post.empty()) return m;
        return "{ " + pre + m + " " + post + "}";
    }

    static bool transfers(const Insn& in) {
        return is(in, {"halt", "loop", "loopi", "bloop", "bloopi", "if", "j", "call", "ret", "rti", "jr", "callr"});
    }
    // Could SR's interrupt enable have just been set?
    static bool may_enable_interrupt(const Insn& in) {
        if (in.name == "sbset" && in.f['i'] == 5) return true;
        if (is(in, {"lri", "lr", "lrr", "lrrd", "lrri", "lrrn", "mrr"}) && in.f['d'] == 0x13) return true;
        return false;
    }

    // ---- reaching the code ----

    const Insn& at(uint16_t a) {
        auto it = code.find(a);
        if (it != code.end()) return it->second;
        if (a >= words.size()) fail(where + ": the code runs to " + hex4(a) + ", past the end of the image");
        Insn in = decode(words, a);
        if (!in.valid) fail(where + ": " + hex4(a) + " holds " + hex4(words[a]) + ", which is no instruction, and is reached");
        return code.emplace(a, std::move(in)).first->second;
    }
    bool in_image(unsigned a) const { return a < words.size(); }

    void reach(std::vector<uint16_t> roots) {
        std::vector<uint16_t> work = std::move(roots);
        for (uint16_t r : work) leaders.insert(r);
        std::set<uint16_t> done;
        auto leader = [&](unsigned a) {
            if (!in_image(a)) return;
            leaders.insert(static_cast<uint16_t>(a));
            work.push_back(static_cast<uint16_t>(a));
        };
        auto next = [&](unsigned a) {
            if (in_image(a)) work.push_back(static_cast<uint16_t>(a));
        };
        while (!work.empty()) {
            const uint16_t a = work.back();
            work.pop_back();
            if (!done.insert(a).second) continue;
            const Insn in = at(a);
            const unsigned after = a + in.size;
            const std::string& n = in.name;
            if (n == "halt") {
                // Execution may go on after the CPU clears the halt; only if that is code anyway.
            } else if (n == "j") {
                if (in.f['a'] < 0x8000) leader(in.f['a']);
                if (conditional(in)) next(after);
            } else if (n == "call") {
                if (in.f['a'] < 0x8000) leader(in.f['a']);
                leader(after);
            } else if (n == "ret" || n == "rti" || n == "jr") {
                if (conditional(in)) next(after);
            } else if (n == "callr") {
                leader(after);
            } else if (n == "if") {
                const Insn skipped = at(static_cast<uint16_t>(after));
                if (is(skipped, {"loop", "loopi", "bloop", "bloopi", "if"})) bad(in, "skips a loop or another IF: not handled");
                tails.insert(skipped.addr);
                next(after);
                next(after + skipped.size);
            } else if (n == "loop" || n == "loopi") {
                const Insn body = at(static_cast<uint16_t>(after));
                if (transfers(body)) bad(in, "repeats a transfer of control: not handled");
                looped.insert(body.addr);
                next(after + body.size);
            } else if (n == "bloop" || n == "bloopi") {
                leader(after);
                loop_ends[static_cast<uint16_t>(in.f['a'])].push_back(static_cast<uint16_t>(after));
                leader(in.f['a'] + 1u);
            } else {
                if (may_enable_interrupt(in)) leader(after);
                else next(after);
            }
        }
        // A LOOP's instruction is part of the LOOP unless something else jumps to it.
        for (auto it = code.begin(); it != code.end();) {
            if (looped.contains(it->first) && !done.contains(it->first)) it = code.erase(it);
            else ++it;
        }
    }

    // ---- which flags are wanted where ----
    //
    // Most instructions set the status flags and almost none of those settings is ever tested.
    // Backward data flow over the lifted code finds the flags wanted after each instruction; an
    // instruction whose flags nobody wants is lifted without them. Where control leaves by a
    // computed address (returns, JR, an interrupt), everything any entry point wants is wanted.

    bool loop_unit(const Insn& in) const { return in.name == "loop" || in.name == "loopi"; }
    unsigned unit_size(const Insn& in) const { return loop_unit(in) ? in.size + decode(words, static_cast<uint16_t>(in.addr + in.size)).size : in.size; }
    uint8_t in_at(unsigned a) const {
        auto it = live_in.find(static_cast<uint16_t>(a));
        return it == live_in.end() ? 0 : it->second;
    }
    uint8_t out_at(unsigned a) const {
        auto it = live_out.find(static_cast<uint16_t>(a));
        return it == live_out.end() ? 0 : it->second;
    }
    void liveness() {
        for (bool changed = true; changed;) {
            changed = false;
            uint8_t anywhere = 0;  // wanted at some entry point
            for (uint16_t l : leaders) anywhere |= in_at(l);
            for (auto it = code.rbegin(); it != code.rend(); ++it) {
                const Insn& in = it->second;
                const std::string& n = in.name;
                const unsigned a = in.addr, after = a + unit_size(in), last = after - 1;
                uint8_t out = 0, uses = flag_uses(in), defs = flag_defs(in);
                bool falls = true;
                if (n == "halt") out = anywhere, falls = false;
                else if (n == "j") {
                    if (in.f['a'] < 0x8000) out |= in_at(in.f['a']);
                    if (in.f['a'] <= static_cast<int>(a)) out |= anywhere;  // (it may wait here, and be interrupted)
                    falls = conditional(in);
                } else if (n == "call") {
                    if (in.f['a'] < 0x8000) out |= in_at(in.f['a']);
                    falls = conditional(in);
                } else if (n == "ret" || n == "rti" || n == "jr" || n == "callr") {
                    out = anywhere;
                    falls = conditional(in);
                } else if (n == "if") {
                    out = out_at(a + 1);  // the skipped instruction's own successors
                } else if (loop_unit(in)) {
                    const Insn body = decode(words, static_cast<uint16_t>(a + in.size));
                    uses |= flag_uses(body);
                    defs = 0;  // it may run no times
                } else if (n == "bloop" || n == "bloopi") {
                    out |= in_at(in.f['a'] + 1u);
                } else if (may_enable_interrupt(in)) {
                    out |= anywhere;
                }
                if (falls) out |= in_at(after);
                if (auto le = loop_ends.find(static_cast<uint16_t>(last)); le != loop_ends.end()) {
                    for (uint16_t start : le->second) out |= in_at(start);
                    out |= in_at(after);
                }
                const uint8_t in_flags = static_cast<uint8_t>(uses | (out & ~defs));
                if (out != out_at(a) || in_flags != in_at(a) || !live_in.contains(static_cast<uint16_t>(a))) {
                    live_out[static_cast<uint16_t>(a)] = out;
                    live_in[static_cast<uint16_t>(a)] = in_flags;
                    changed = true;
                }
            }
        }
    }

    // ---- the function ----

    std::string label(unsigned a) const { return "L_" + hex4(a); }
    std::string go(unsigned target) const {
        if (target >= 0x8000) return "{ s.pc = 0x" + hex4(target) + "; s.r = r; return; }";  // the ROM: run() hands it to the host
        if (!in_image(target) || !code.contains(static_cast<uint16_t>(target))) return "{ s.pc = 0x" + hex4(target) + "; s.bad_pc(); }";
        return "goto " + label(target) + ";";
    }
    // A jump as the instruction at `from` takes it: backward ones may be the DSP waiting.
    std::string jump(unsigned from, unsigned target) const {
        if (target <= from && target < 0x8000 && code.contains(static_cast<uint16_t>(target)))
            return "{ if (s.attention()) { if (s.stop.load()) { s.pc = 0x" + hex4(target) + "; s.r = r; return; } if (r.sr & 0x0800) { r.sr = s.take_int(r.sr, 0x" +
                   hex4(target) + "); goto " + label(0x000E) + "; } } if (s.maybe_idle(0x" + hex4(from) + ")) { s.r = r; if (s.idle_slow()) { s.pc = 0x" +
                   hex4(target) + "; return; } } goto " + label(target) + "; }";
        return go(target);
    }

    // What follows an instruction whose last word is `last`: the end of a block loop, perhaps.
    std::string loop_check(unsigned last) const {
        auto it = loop_ends.find(static_cast<uint16_t>(last));
        if (it == loop_ends.end()) return "";
        std::string out = "    if (s.loop_hit(0x" + hex4(last) + ") && s.loop_again()) { ";
        if (it->second.size() == 1) out += "if (s.loop_start() == 0x" + hex4(it->second[0]) + ") goto " + label(it->second[0]) + "; ";
        out += "s.pc = s.loop_start(); goto dispatch; }\n";
        return out;
    }

    std::string emit_function(const std::string& fn) {
        liveness();
        const std::string interrupt = "if (s.int_due(r.sr)) { r.sr = s.take_int(r.sr, s.pc); s.pc = 0x000E; }";
        std::ostringstream o;
        o << "void " << fn << "(Core& s) {\n    Regs r = s.r;\n";
        o << "dispatch:\n    switch (s.pc) {\n";
        for (uint16_t l : leaders)
            if (code.contains(l)) o << "        case 0x" << hex4(l) << ": goto " << label(l) << ";\n";
        o << "        default: s.r = r; s.bad_pc();\n    }\n";
        for (auto it = code.begin(); it != code.end(); ++it) {
            const Insn& in = it->second;
            const unsigned a = in.addr, after = a + in.size;
            unsigned last = after - 1;      // the unit's last word
            unsigned unit_end = after;      // the address after the unit
            const std::string& n = in.name;
            const uint8_t live = out_at(a);
            std::string body;
            bool falls = true;
            if (n == "halt") {
                body = "{ s.halted = true; s.pc = 0x" + hex4(after) + "; s.r = r; return; }";
                falls = false;
            } else if (n == "j") {
                if (conditional(in)) body = "if (" + cond(in.f['c']) + ") " + jump(a, in.f['a']);
                else body = jump(a, in.f['a']), falls = false;
            } else if (n == "call") {
                const std::string call = "{ s.push(0, 0x" + hex4(after) + "); " + go(in.f['a']) + " }";
                if (conditional(in)) body = "if (" + cond(in.f['c']) + ") " + call;
                else body = call, falls = false;
            } else if (n == "ret") {
                const std::string ret = "{ s.pc = s.pop(0); goto dispatch; }";
                if (conditional(in)) body = "if (" + cond(in.f['c']) + ") " + ret;
                else body = ret, falls = false;
            } else if (n == "rti") {
                const std::string rti = "{ r.sr = s.pop(1) & ~0x0100; s.pc = s.pop(0); " + interrupt + " goto dispatch; }";
                if (conditional(in)) body = "if (" + cond(in.f['c']) + ") " + rti;
                else body = rti, falls = false;
            } else if (n == "jr") {
                const std::string jr = "{ s.pc = " + rread(in, in.f['r'], false) + "; goto dispatch; }";
                if (conditional(in)) body = "if (" + cond(in.f['c']) + ") " + jr;
                else body = jr, falls = false;
            } else if (n == "callr") {
                const std::string callr = "{ s.push(0, 0x" + hex4(after) + "); s.pc = " + rread(in, in.f['r'], false) + "; goto dispatch; }";
                if (conditional(in)) body = "if (" + cond(in.f['c']) + ") " + callr;
                else body = callr, falls = false;
            } else if (n == "if") {
                body = "if (!" + cond(in.f['c']) + ") goto T_" + hex4(after) + ";";
            } else if (n == "loop" || n == "loopi") {
                const Insn inner = decode(words, static_cast<uint16_t>(after));
                const std::string count = n == "loop" ? rread(in, in.f['r'], true) : num(in.f['i']);
                body = "for (unsigned n_ = " + count + "; n_; --n_) { " + plain(inner, live) + " }";
                last = after + inner.size - 1;
                unit_end = after + inner.size;
                // If the repeated instruction is also lifted on its own, it follows here: step over it.
                if (code.contains(static_cast<uint16_t>(after))) body += " " + go(unit_end);
                if (code.contains(static_cast<uint16_t>(after))) falls = false;
            } else if (n == "bloop" || n == "bloopi") {
                const unsigned end = in.f['a'];
                const bool constant = n == "bloopi";
                const std::string count = constant ? num(in.f['i']) : rread(in, in.f['r'], true);
                // No rounds: on to the instruction after the block, if the block ends on a whole one.
                std::string none;
                if (constant && in.f['i'] != 0) none = "";
                else if (loop_ends_whole(end)) none = "if (n_ == 0) " + go(end + 1) + " ";
                else none = "if (n_ == 0) s.die(\"a block loop of no rounds at %04X ends inside an instruction: what the DSP does is not known\", 0x" + hex4(a) + "); ";
                body = "{ const uint16_t n_ = " + count + "; " + none + "s.loop_begin(0x" + hex4(after) + ", 0x" + hex4(end) + ", n_); }";
            } else {
                body = plain(in, live);
                if (may_enable_interrupt(in))
                    body += " if (s.int_due(r.sr)) { r.sr = s.take_int(r.sr, 0x" + hex4(after) + "); goto " + label(0x000E) + "; }";
            }
            o << label(a) << ": ;\n";
            o << "    /* " << hex4(a) << " " << n << (in.ext.empty() ? "" : "'" + in.ext) << " */ " << body << "\n";
            if (tails.contains(static_cast<uint16_t>(a))) o << "T_" << hex4(a) << ": ;\n";
            o << loop_check(last);
            // Onward: to the next instruction lifted, which must be the one that follows.
            auto nx = std::next(it);
            const bool reachable_after = falls || tails.contains(static_cast<uint16_t>(a)) || loop_ends.contains(static_cast<uint16_t>(last));
            if (reachable_after && (nx == code.end() || nx->first != unit_end)) o << "    " << go(unit_end) << "\n";
        }
        o << "}\n";
        return o.str();
    }
    bool loop_ends_whole(unsigned end) const {
        // The block's last word is a one-word instruction (which may be a LOOP's body).
        if (end >= words.size()) return false;
        for (const auto& [addr, in] : code) {
            if (addr + in.size - 1 == end && in.size == 1) return true;
            if ((in.name == "loop" || in.name == "loopi") && addr + 1u == end) return true;
        }
        return false;
    }
};

// ---- the microcodes this knows ----

struct Known {
    uint32_t hash;
    const char* name;
    uint32_t dol_address, bytes;
    std::vector<uint16_t> roots;  // targets of computed jumps
};
uint32_t ector_hash(const std::vector<uint8_t>& data) {
    uint32_t h = 0;
    for (uint8_t b : data) {
        h ^= b;
        h = (h << 3) | (h >> 29);
    }
    return h;
}
std::vector<Known> known() {
    std::vector<Known> k;
    // Mario Kart: Double Dash!! (GM4E01 rev 0): JAudio's microcode, the object `jdsp` of dsptask.c.
    // Computed jumps: the command table at 0x0075 (two words an entry, sixteen entries, reached with
    // JR at 0x0042; 0x0073 is the same for command 3), the oscillators' at 0x0891 (CALLR at 0x088C),
    // the four answers to "frame done" whose addresses are data at 0x05FF (0x0617, 0x0618, 0x0658,
    // 0x065B), and the two places the last of those returns to (0x002D, 0x06C5).
    Known jdsp{0x2FCDF1EC, "jaudio", 0x80343680, 0x1D20, {0x0067, 0x002D, 0x06C5, 0x0617, 0x0618, 0x0658, 0x065B, 0x0073}};
    for (uint16_t a = 0x0075; a <= 0x0093; a += 2) jdsp.roots.push_back(a);
    for (uint16_t a = 0x0891; a <= 0x08AF; a += 2) jdsp.roots.push_back(a);
    k.push_back(std::move(jdsp));
    return k;
}

std::string lift_one(const std::vector<uint16_t>& words, const std::vector<uint16_t>& extra_roots, const std::string& where,
                     const std::string& fn, std::string* summary) {
    Lifter l;
    l.words = words;
    l.where = where;
    std::vector<uint16_t> roots;
    for (uint16_t v = 0; v <= 0x10 && v < words.size(); v += 2) roots.push_back(v);
    for (uint16_t r : extra_roots) {
        if (r >= words.size()) fail(where + ": root " + hex4(r) + " is outside the image");
        roots.push_back(r);
    }
    l.reach(roots);
    std::string text = l.emit_function(fn);
    if (summary)
        *summary = num(static_cast<long long>(l.code.size())) + " instructions lifted, " + num(static_cast<long long>(l.leaders.size())) +
                   " entry points; flags computed for " + num(l.flagged) + " of " + num(l.flagged + l.quiet) + " instructions that set them";
    return text;
}

std::vector<uint16_t> parse_words(const std::string& path) {
    std::ifstream in(qilin::native_path(path));
    if (!in) fail("can't read " + path);
    std::vector<uint16_t> words;
    std::string line;
    while (std::getline(in, line)) {
        const auto cut = line.find(';');
        if (cut != std::string::npos) line.erase(cut);
        std::istringstream ls(line);
        std::string tok;
        while (ls >> tok) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(tok.c_str(), &end, 16);
            if (*end || v > 0xFFFF) fail(path + ": '" + tok + "' is not a 16-bit hexadecimal word");
            words.push_back(static_cast<uint16_t>(v));
        }
    }
    return words;
}

}  // namespace

int main(int raw_argc, char** raw_argv) {
    // UTF-8 arguments on every system (Windows: from the wide command line),
    // and paths made from them as UTF-8: a folder with any name works.
    int argc = raw_argc;
    char** argv = raw_argv;
    std::optional<qilin::CommandLine> command_line;
    try {
        command_line.emplace(raw_argc, raw_argv);
        argc = command_line->count();
        argv = command_line->values();
    } catch (const std::exception& e) {
        fail(e.what());
    }
    std::string input, output, roots_text;
    bool raw = false;
    if (argc < 2 || std::string_view(argv[1]) != "lift") {
        std::fprintf(stderr, "usage: qilin-dsp lift <main.dol> -o <out.cpp>\n"
                             "       qilin-dsp lift --words <file> [--roots a,b,...] -o <out.cpp>\n");
        return 2;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if ((a == "-o" || a == "--out") && i + 1 < argc) output = argv[++i];
        else if (a == "--words" && i + 1 < argc) raw = true, input = argv[++i];
        else if (a == "--roots" && i + 1 < argc) roots_text = argv[++i];
        else if (input.empty()) input = argv[i];
        else fail("unexpected argument " + std::string(a));
    }
    if (input.empty() || output.empty()) fail("an input and -o <out.cpp> are needed");

    std::ostringstream out;
    out << "// Generated by qilin-dsp from the player's own files: the game's DSP microcode as C++.\n"
           "// Never to be committed or distributed.\n"
           "#include \"dsp_core.hpp\"\n\nnamespace {\n\nusing namespace mkdd::dsp;\n\n";
    std::string table;
    if (raw) {
        const std::vector<uint16_t> words = parse_words(input);
        std::vector<uint8_t> bytes;
        for (uint16_t w : words) bytes.push_back(static_cast<uint8_t>(w >> 8)), bytes.push_back(static_cast<uint8_t>(w));
        std::vector<uint16_t> roots;
        std::istringstream rs(roots_text);
        for (std::string tok; std::getline(rs, tok, ',');)
            if (!tok.empty()) roots.push_back(static_cast<uint16_t>(std::strtoul(tok.c_str(), nullptr, 16)));
        std::string summary;
        out << lift_one(words, roots, input, "run_words", &summary) << "\n";
        char row[160];
        std::snprintf(row, sizeof row, "    {0x%08Xu, %zu, run_words, \"words\"},\n", ector_hash(bytes), bytes.size());
        table += row;
        std::printf("qilin-dsp: %s: %s\n", input.c_str(), summary.c_str());
    } else {
        auto dol = qilin::Dol::load(qilin::native_path(input));
        if (!dol) fail(dol.error());
        unsigned found = 0;
        for (const Known& k : known()) {
            if (!dol->mapped(k.dol_address, k.bytes)) continue;
            const auto image = dol->image().subspan(k.dol_address - dol->base(), k.bytes);
            const std::vector<uint8_t> bytes(image.begin(), image.end());
            if (ector_hash(bytes) != k.hash) continue;
            std::vector<uint16_t> words(k.bytes / 2);
            for (size_t i = 0; i < words.size(); ++i) words[i] = static_cast<uint16_t>((bytes[2 * i] << 8) | bytes[2 * i + 1]);
            char fn[32];
            std::snprintf(fn, sizeof fn, "run_%08x", k.hash);
            std::string summary;
            out << lift_one(words, k.roots, k.name, fn, &summary) << "\n";
            char row[160];
            std::snprintf(row, sizeof row, "    {0x%08Xu, %u, %s, \"%s\"},\n", k.hash, k.bytes, fn, k.name);
            table += row;
            std::printf("qilin-dsp: microcode %08X (%s) at %08X: %s\n", k.hash, k.name, k.dol_address, summary.c_str());
            ++found;
        }
        if (!found) fail("no microcode this knows is in " + input + " (is it the supported build?)");
    }
    out << "const mkdd::dsp::Program kPrograms[] = {\n" << table << "};\n\n}  // namespace\n\n"
        << "namespace mkdd::dsp {\nconst Program* const g_programs = kPrograms;\n"
        << "const size_t g_program_count = sizeof kPrograms / sizeof kPrograms[0];\n}\n";

    const std::string text = out.str();
    const std::filesystem::path path = qilin::native_path(output);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    {   // Leave an unchanged file alone, so nothing rebuilds for it.
        std::ifstream old(path, std::ios::binary);
        std::stringstream have;
        have << old.rdbuf();
        if (old && have.str() == text) return 0;
    }
    std::ofstream f(path, std::ios::binary);
    f << text;
    if (!f) fail("can't write " + output);
    return 0;
}
