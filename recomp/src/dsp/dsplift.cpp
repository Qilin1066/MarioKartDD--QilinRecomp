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
// how that is known). It is generated into the build tree and never kept: it
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

const char* kCond[16] = {
    "!(((sr_ >> 1) ^ (sr_ >> 3)) & 1)",                     // GE: O == S
    "(((sr_ >> 1) ^ (sr_ >> 3)) & 1)",                      // L
    "(!(((sr_ >> 1) ^ (sr_ >> 3)) & 1) && !(sr_ & 4))",     // G
    "((((sr_ >> 1) ^ (sr_ >> 3)) & 1) || (sr_ & 4))",       // LE
    "!(sr_ & 4)",                                           // NZ
    "(sr_ & 4)",                                            // Z
    "!(sr_ & 1)",                                           // NC
    "(sr_ & 1)",                                            // C
    "!(sr_ & 0x10)",                                        // below s32
    "(sr_ & 0x10)",                                         // above s32
    "((sr_ & 0x30) && !(sr_ & 4))",
    "(!(sr_ & 0x30) || (sr_ & 4))",
    "!(sr_ & 0x40)",                                        // LNZ
    "(sr_ & 0x40)",                                         // LZ
    "(sr_ & 2)",                                            // O
    "true",
};
std::string cond(int cc) {
    if (cc == 15) return "true";
    return "[&] { const unsigned sr_ = s.r.sr; return static_cast<bool>(" + std::string(kCond[cc]) + "); }()";
}

struct Lifter {
    std::vector<uint16_t> words;
    std::map<uint16_t, Insn> code;        // reached instructions
    std::set<uint16_t> leaders;           // addresses the dispatcher knows
    std::set<uint16_t> looped;            // instructions repeated by a LOOP before them
    std::map<uint16_t, std::vector<uint16_t>> loop_ends;  // last word of a block loop's body -> its starts
    std::set<uint16_t> tails;             // instructions whose tail label is jumped to
    std::string where;                    // for messages

    [[noreturn]] void bad(const Insn& in, const std::string& what) const {
        fail(where + ": " + hex4(in.addr) + " (" + in.name + (in.ext.empty() ? "" : "'" + in.ext) + "): " + what);
    }

    // Registers by number, as the generic moves and loads see them.
    std::string rread(const Insn& in, int n, bool saturate) const {
        if (n < 4) return "s.r.ar[" + num(n) + "]";
        if (n < 8) return "s.r.ix[" + num(n - 4) + "]";
        if (n < 12) return "s.r.wr[" + num(n - 8) + "]";
        switch (n) {
            case 0x10: case 0x11: return "s.ach(" + num(n - 0x10) + ")";
            case 0x12: return "s.r.config";
            case 0x13: return "s.r.sr";
            case 0x18: case 0x19: return "s.r.axl[" + num(n - 0x18) + "]";
            case 0x1A: case 0x1B: return "s.r.axh[" + num(n - 0x1A) + "]";
            case 0x1C: case 0x1D: return "s.acl(" + num(n - 0x1C) + ")";
            case 0x1E: case 0x1F: return std::string(saturate ? "s.acm_sat(" : "s.acm(") + num(n - 0x1E) + ")";
            default: bad(in, "reads register " + num(n) + " (a stack or a part of the product): not handled");
        }
    }
    std::string rwrite(const Insn& in, int n, const std::string& v) const {
        const std::string u = "static_cast<uint16_t>(" + v + ")";
        if (n < 4) return "s.r.ar[" + num(n) + "] = " + u + ";";
        if (n < 8) return "s.r.ix[" + num(n - 4) + "] = " + u + ";";
        if (n < 12) return "s.r.wr[" + num(n - 8) + "] = " + u + ";";
        switch (n) {
            case 0x10: case 0x11: return "s.set_ach(" + num(n - 0x10) + ", " + u + ");";
            case 0x12: return "s.r.config = " + u + " & 0xFF;";
            case 0x13: return "s.set_sr(" + u + ");";
            case 0x18: case 0x19: return "s.r.axl[" + num(n - 0x18) + "] = " + u + ";";
            case 0x1A: case 0x1B: return "s.r.axh[" + num(n - 0x1A) + "] = " + u + ";";
            case 0x1C: case 0x1D: return "s.set_acl(" + num(n - 0x1C) + ", " + u + ");";
            case 0x1E: case 0x1F: return "s.set_acm(" + num(n - 0x1E) + ", " + u + ");";
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

    // The extension: what it does before the instruction (loads, stores, new addresses, all from
    // the state as it was) and what it writes after.
    void extension(const Insn& in, std::string& pre, std::string& post) const {
        const std::string& x = in.ext;
        if (x.empty()) return;
        const Fields& e = in.e;
        auto ar = [](int i) { return "s.r.ar[" + num(i) + "]"; };
        auto step = [&](int i, bool by_index) {
            return by_index ? "s.ar_add(" + num(i) + ", s.r.ix[" + num(i) + "])" : "s.ar_inc(" + num(i) + ")";
        };
        auto set_ar = [&](int i, const std::string& name, bool by_index) {
            pre += "const uint16_t " + name + " = " + step(i, by_index) + "; ";
            post += ar(i) + " = " + name + "; ";
        };
        if (x == "dr") {
            pre += "const uint16_t a_ = s.ar_dec(" + num(e['r']) + "); ";
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
            pre += "s.wr(" + ar(3) + ", s.acm_sat(" + num(e['s']) + ")); const uint16_t v_ = s.rd(" + ar(0) + "); ";
            set_ar(3, "a3_", x == "lsm" || x == "lsnm");
            set_ar(0, "a0_", x == "lsn" || x == "lsnm");
            post += rwrite(in, 0x18 + e['d'], "v_") + " ";
        } else if (x == "sl" || x == "sln" || x == "slm" || x == "slnm") {
            pre += "s.wr(" + ar(0) + ", s.acm_sat(" + num(e['s']) + ")); const uint16_t v_ = s.rd(" + ar(3) + "); ";
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
            post += "s.r.axh[" + num(e['r']) + "] = v_; s.r.axl[" + num(e['r']) + "] = w_; ";
        } else {
            bad(in, "extension not handled");
        }
    }

    // An instruction that is not a transfer of control, as statements.
    std::string plain(const Insn& in) const {
        const Fields& f = in.f;
        const std::string& n = in.name;
        const std::string d = num(f['d']), sx = num(f['s']), t = num(f['t']);
        const std::string A = "s.r.ac[" + d + "]";
        const std::string other = "s.r.ac[" + num(1 - f['d']) + "]";
        auto s16 = [](const std::string& v) { return "(int64_t(static_cast<int16_t>(" + v + ")) << 16)"; };
        const std::string imm = num(f['i']);
        const std::string imm16 = "(int64_t(" + num(static_cast<int16_t>(f['i'])) + ") << 16)";
        const std::string imm8 = "(int64_t(" + num(static_cast<int8_t>(f['i'])) + ") << 16)";
        const std::string short_address = "static_cast<uint16_t>((s.r.config << 8) | " + num(f['m']) + ")";
        auto ar = [](int i) { return "s.r.ar[" + num(i) + "]"; };
        std::string m;  // the instruction itself

        if (n == "nop" || n == "nx") m = "";
        else if (n == "dar") m = ar(f['d']) + " = s.ar_dec(" + d + ");";
        else if (n == "iar") m = ar(f['d']) + " = s.ar_inc(" + d + ");";
        else if (n == "subarn") m = ar(f['d']) + " = s.ar_sub(" + d + ", s.r.ix[" + d + "]);";
        else if (n == "addarn") m = ar(f['d']) + " = s.ar_add(" + d + ", s.r.ix[" + sx + "]);";
        else if (n == "lri") m = rwrite(in, f['d'], imm);
        else if (n == "lr") m = rwrite(in, f['d'], "s.rd(" + num(f['m']) + ")");
        else if (n == "sr") m = "s.wr(" + num(f['m']) + ", " + rread(in, f['s'], true) + ");";
        else if (n == "addi") m = A + " = s.add(" + A + ", " + imm16 + ");";
        else if (n == "xori") m = "s.logic(" + d + ", s.acm(" + d + ") ^ " + imm + ");";
        else if (n == "andi") m = "s.logic(" + d + ", s.acm(" + d + ") & " + imm + ");";
        else if (n == "ori") m = "s.logic(" + d + ", s.acm(" + d + ") | " + imm + ");";
        else if (n == "cmpi") m = "(void)s.sub(" + A + ", " + imm16 + ");";
        else if (n == "andf") m = "s.r.sr = (s.acm(" + d + ") & " + imm + ") == 0 ? (s.r.sr | 0x40) : (s.r.sr & ~0x40);";
        else if (n == "andcf")
            m = "s.r.sr = (s.acm(" + d + ") & " + imm + ") == " + imm + " ? (s.r.sr | 0x40) : (s.r.sr & ~0x40);";
        else if (n == "ilrr" || n == "ilrrd" || n == "ilrri" || n == "ilrrn") {
            const std::string step = n == "ilrrd" ? ar(f['s']) + " = s.ar_dec(" + sx + "); "
                                   : n == "ilrri" ? ar(f['s']) + " = s.ar_inc(" + sx + "); "
                                   : n == "ilrrn" ? ar(f['s']) + " = s.ar_add(" + sx + ", s.r.ix[" + sx + "]); " : "";
            m = "{ const uint16_t i_ = s.irom(" + ar(f['s']) + "); s.set_acm(" + d + ", i_); " + step + "}";
        } else if (n == "lsrn") m = "s.r.ac[0] = s.lsrn(s.r.ac[0], s.acm(1));";
        else if (n == "asrn") m = "s.r.ac[0] = s.asrn(s.r.ac[0], s.acm(1));";
        else if (n == "addis") m = A + " = s.add(" + A + ", " + imm8 + ");";
        else if (n == "cmpis") m = "(void)s.sub(" + A + ", " + imm8 + ");";
        else if (n == "lris") m = rwrite(in, 0x18 + f['d'], num(static_cast<uint16_t>(static_cast<int8_t>(f['i']))));
        else if (n == "sbclr") m = "s.r.sr &= ~" + num(1ll << (f['i'] + 6)) + ";";
        else if (n == "sbset") m = "s.r.sr |= " + num(1ll << (f['i'] + 6)) + ";";
        else if (n == "lsl" || n == "asl") m = "s.r.ac[" + num(f['r']) + "] = s.lsl(s.r.ac[" + num(f['r']) + "], " + imm + ");";
        else if (n == "lsr" || n == "asr") {
            const std::string acc = "s.r.ac[" + num(f['r']) + "]";
            m = acc + " = s." + n + "(" + acc + ", " + num(f['i'] ? 64 - f['i'] : 0) + ");";
        } else if (n == "si") m = "s.hw_write(" + num(0xFF00 | f['m']) + ", " + imm + ");";
        else if (n == "lrr" || n == "lrrd" || n == "lrri" || n == "lrrn") {
            const std::string step = n == "lrrd" ? ar(f['s']) + " = s.ar_dec(" + sx + "); "
                                   : n == "lrri" ? ar(f['s']) + " = s.ar_inc(" + sx + "); "
                                   : n == "lrrn" ? ar(f['s']) + " = s.ar_add(" + sx + ", s.r.ix[" + sx + "]); " : "";
            m = "{ const uint16_t v_ = s.rd(" + ar(f['s']) + "); " + rwrite(in, f['d'], "v_") + " " + step + "}";
        } else if (n == "srr" || n == "srrd" || n == "srri" || n == "srrn") {
            const std::string step = n == "srrd" ? ar(f['d']) + " = s.ar_dec(" + d + ");"
                                   : n == "srri" ? ar(f['d']) + " = s.ar_inc(" + d + ");"
                                   : n == "srrn" ? ar(f['d']) + " = s.ar_add(" + d + ", s.r.ix[" + d + "]);" : "";
            m = "s.wr(" + ar(f['d']) + ", " + rread(in, f['s'], true) + "); " + step;
        } else if (n == "mrr") m = "{ const uint16_t v_ = " + rread(in, f['s'], true) + "; " + rwrite(in, f['d'], "v_") + " }";
        else if (n == "lrs") m = rwrite(in, 0x18 + f['d'], "s.rd(" + short_address + ")");
        else if (n == "srsh") m = "s.wr(" + short_address + ", s.ach(" + sx + "));";
        else if (n == "srs") m = "s.wr(" + short_address + ", " + rread(in, 0x1C + f['s'], true) + ");";
        else if (n == "xorr") m = "s.logic(" + d + ", s.acm(" + d + ") ^ s.r.axh[" + sx + "]);";
        else if (n == "andr") m = "s.logic(" + d + ", s.acm(" + d + ") & s.r.axh[" + sx + "]);";
        else if (n == "orr") m = "s.logic(" + d + ", s.acm(" + d + ") | s.r.axh[" + sx + "]);";
        else if (n == "andc") m = "s.logic(" + d + ", s.acm(" + d + ") & s.acm(" + num(1 - f['d']) + "));";
        else if (n == "orc") m = "s.logic(" + d + ", s.acm(" + d + ") | s.acm(" + num(1 - f['d']) + "));";
        else if (n == "xorc") m = "s.logic(" + d + ", s.acm(" + d + ") ^ s.acm(" + num(1 - f['d']) + "));";
        else if (n == "not") m = "s.logic(" + d + ", static_cast<uint16_t>(~s.acm(" + d + ")));";
        else if (n == "lsrnrx") m = A + " = s.lsrn(" + A + ", s.r.axh[" + sx + "]);";
        else if (n == "asrnrx") m = A + " = s.asrn(" + A + ", s.r.axh[" + sx + "]);";
        else if (n == "lsrnr") m = A + " = s.lsrn(" + A + ", s.acm(" + num(1 - f['d']) + "));";
        else if (n == "asrnr") m = A + " = s.asrn(" + A + ", s.acm(" + num(1 - f['d']) + "));";
        else if (n == "addr") m = A + " = s.add(" + A + ", " + s16(rread(in, 0x18 + f['s'], false)) + ");";
        else if (n == "addax") m = A + " = s.add(" + A + ", s.ax(" + sx + "));";
        else if (n == "add") m = A + " = s.add(" + A + ", " + other + ");";
        else if (n == "addp") m = A + " = s.add(" + A + ", s.r.prod);";
        else if (n == "subr") m = A + " = s.sub(" + A + ", " + s16(rread(in, 0x18 + f['s'], false)) + ");";
        else if (n == "subax") m = A + " = s.sub(" + A + ", s.ax(" + sx + "));";
        else if (n == "sub") m = A + " = s.sub(" + A + ", " + other + ");";
        else if (n == "subp") m = A + " = s.sub(" + A + ", s.r.prod);";
        else if (n == "movr") m = A + " = s.mov(" + s16(rread(in, 0x18 + f['s'], false)) + ");";
        else if (n == "movax") m = A + " = s.mov(s.ax(" + sx + "));";
        else if (n == "mov") m = A + " = s.mov(" + other + ");";
        else if (n == "movp") m = A + " = s.mov(s.r.prod);";
        else if (n == "movnp") m = A + " = s.mov(-s.r.prod);";
        else if (n == "movpz") m = A + " = s.mov(s.prod_rounded());";
        else if (n == "addaxl") m = A + " = s.add(" + A + ", int64_t(s.r.axl[" + sx + "]));";
        else if (n == "incm") m = A + " = s.add(" + A + ", 0x10000);";
        else if (n == "inc") m = A + " = s.add(" + A + ", 1);";
        else if (n == "decm") m = A + " = s.sub(" + A + ", 0x10000);";
        else if (n == "dec") m = A + " = s.sub(" + A + ", 1);";
        else if (n == "neg") m = A + " = s.neg(" + A + ");";
        else if (n == "abs") m = "if (" + A + " < 0) " + A + " = mkdd::dsp::Core::sx40(-" + A + "); s.flags64(" + A + ");";
        else if (n == "clr") m = A + " = s.mov(0);";
        else if (n == "tst") m = "s.flags64(" + A + ");";
        else if (n == "cmp") m = "(void)s.sub(s.r.ac[0], s.r.ac[1]);";
        else if (n == "cmpaxh") m = "(void)s.sub(s.r.ac[" + sx + "], " + s16("s.r.axh[" + num(f['r']) + "]") + ");";
        else if (n == "clrp") m = "s.r.prod = 0;";
        else if (n == "tstprod") m = "s.flags64(s.r.prod);";
        else if (n == "tstaxh") m = "s.flags16(s.r.axh[" + num(f['r']) + "], false);";
        else if (n == "m2") m = "s.r.sr &= ~0x2000;";
        else if (n == "m0") m = "s.r.sr |= 0x2000;";
        else if (n == "clr15") m = "s.r.sr &= ~0x8000;";
        else if (n == "set15") m = "s.r.sr |= 0x8000;";
        else if (n == "set16") m = "s.r.sr &= ~0x4000;";
        else if (n == "set40") m = "s.r.sr |= 0x4000;";
        else if (n == "asr16") m = A + " = s.asr(" + A + ", 16);";
        else if (n == "lsl16") m = A + " = s.lsl(" + A + ", 16);";
        else if (n == "lsr16") m = A + " = s.lsr(" + A + ", 16);";
        else if (n == "clrl")
            m = A + " = s.mov(((" + A + " & 0x10000) ? " + A + " + 0x8000 : " + A + " + 0x7FFF) & ~int64_t(0xFFFF));";
        else if (n == "addpaxz")
            m = "{ const int64_t p_ = s.prod_rounded(); const int64_t r_ = mkdd::dsp::Core::sx40(p_ + " + s16("s.r.axh[" + sx + "]") +
                "); s.flags64(r_, static_cast<uint64_t>(s.r.prod) > static_cast<uint64_t>(r_)); " + A + " = r_; }";
        else {
            // The multiplier's family: what is multiplied, and what happens to the old product.
            std::string product;
            std::string base = n;
            if (n.starts_with("mulx") || n == "maddx" || n == "msubx") product = "s.mulx(" + sx + ", " + t + ")";
            else if (n.starts_with("mulc") || n == "maddc" || n == "msubc") product = "s.mul(s.acm(" + sx + "), s.r.axh[" + t + "])";
            else if (n == "mulaxh") product = "s.mul(s.r.axh[0], s.r.axh[0])";
            else if (n.starts_with("mul") || n == "madd" || n == "msub") product = "s.mul(s.r.axl[" + sx + "], s.r.axh[" + sx + "])";
            else bad(in, "instruction not handled");
            if (n.starts_with("madd")) m = "s.set_prod(s.r.prod + " + product + ");";
            else if (n.starts_with("msub")) m = "s.set_prod(s.r.prod - " + product + ");";
            else if (n.ends_with("mvz")) m = "{ const int64_t p_ = s.prod_rounded(); s.set_prod(" + product + "); " + A + " = s.mov(p_); }";
            else if (n.ends_with("mv")) m = "{ const int64_t p_ = s.r.prod; s.set_prod(" + product + "); " + A + " = s.mov(p_); }";
            else if (n.ends_with("ac")) m = "{ const int64_t p_ = " + A + " + s.r.prod; s.set_prod(" + product + "); " + A + " = s.mov(p_); }";
            else m = "s.set_prod(" + product + ");";
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

    // ---- the function ----

    std::string label(unsigned a) const { return "L_" + hex4(a); }
    std::string go(unsigned target) const {
        if (target >= 0x8000) return "{ s.pc = 0x" + hex4(target) + "; return; }";  // the ROM: run() hands it to the host
        if (!in_image(target) || !code.contains(static_cast<uint16_t>(target))) return "{ s.pc = 0x" + hex4(target) + "; s.bad_pc(); }";
        return "goto " + label(target) + ";";
    }
    // A jump as the instruction at `from` takes it: backward ones may be the DSP waiting.
    std::string jump(unsigned from, unsigned target) const {
        if (target <= from && target < 0x8000 && code.contains(static_cast<uint16_t>(target)))
            return "{ if (s.idle(0x" + hex4(from) + ")) { s.pc = 0x" + hex4(target) + "; return; } goto " + label(target) + "; }";
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
        std::ostringstream o;
        o << "void " << fn << "(mkdd::dsp::Core& s) {\n";
        o << "dispatch:\n    switch (s.pc) {\n";
        for (uint16_t l : leaders)
            if (code.contains(l)) o << "        case 0x" << hex4(l) << ": goto " << label(l) << ";\n";
        o << "        default: s.bad_pc();\n    }\n";
        for (auto it = code.begin(); it != code.end(); ++it) {
            const Insn& in = it->second;
            const unsigned a = in.addr, after = a + in.size;
            unsigned last = after - 1;      // the unit's last word
            unsigned unit_end = after;      // the address after the unit
            const std::string& n = in.name;
            std::string body;
            bool falls = true;
            if (n == "halt") {
                body = "s.halted = true; s.pc = 0x" + hex4(after) + "; return;";
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
                const std::string rti = "{ s.set_sr(s.pop(1)); s.pc = s.pop(0); if (s.int_due()) { s.take_int(s.pc); s.pc = 0x000E; } goto dispatch; }";
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
                body = "for (unsigned n_ = " + count + "; n_; --n_) { " + plain(inner) + " }";
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
                body = plain(in);
                if (may_enable_interrupt(in))
                    body += " if (s.int_due()) { s.take_int(0x" + hex4(after) + "); s.pc = 0x000E; goto dispatch; }";
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
    if (summary) *summary = num(static_cast<long long>(l.code.size())) + " instructions lifted, " + num(static_cast<long long>(l.leaders.size())) + " entry points";
    return l.emit_function(fn);
}

std::vector<uint16_t> parse_words(const std::string& path) {
    std::ifstream in(path);
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

int main(int argc, char** argv) {
    std::string input, output, roots_text;
    bool raw = false;
    if (argc < 2 || std::string_view(argv[1]) != "lift") {
        std::fprintf(stderr, "usage: qilin-dsp lift <main.dol> -o <out.cpp>\n"
                             "       qilin-dsp lift --words <file> [--roots a,b,...] -o <out.cpp>\n");
        return 2;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-o" && i + 1 < argc) output = argv[++i];
        else if (a == "--words" && i + 1 < argc) raw = true, input = argv[++i];
        else if (a == "--roots" && i + 1 < argc) roots_text = argv[++i];
        else if (input.empty()) input = argv[i];
        else fail("unexpected argument " + std::string(a));
    }
    if (input.empty() || output.empty()) fail("an input and -o <out.cpp> are needed");

    std::ostringstream out;
    out << "// Generated by qilin-dsp from the player's own files: the game's DSP microcode as C++.\n"
           "// Never to be committed or distributed.\n"
           "#include \"dsp_core.hpp\"\n\nnamespace {\n\n";
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
        auto dol = qilin::Dol::load(input);
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
    std::filesystem::path path(output);
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
