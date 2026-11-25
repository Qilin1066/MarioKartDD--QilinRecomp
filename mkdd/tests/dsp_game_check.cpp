// mkdd_dsp_check: the game's lifted DSP microcode driven without the game.
//
//   mkdd_dsp_check <main.dol> [--wav out.wav] [--frames n] [--voices n] [--pitch hex]
//                  [--osc n | --aw <file> --offset n --bytes n --format 0-3 --samples n [--loop start end]]
//
// Built only with the player's DOL (the microcode's C++ is generated from it).
// It plays the CPU's part as JAudio's dsptask.c/dspproc.c play it: the boot
// handshake, DsetupTable with the DOL's own filter tables, then one DsyncFrame
// a frame, releasing the channels sixteen at a time, with `voices` channel
// blocks set up as JASDsp::TChannel::playStart/setWaveInfo/setOscInfo set them
// (layout: doldecomp/mkdd's JASDsp.h, CC0). The sound is a built-in oscillator
// (--osc, the default) or a wave from the disc's archives put in ARAM (the
// numbers come from tools/analysis/dsp_waves.py). It writes the mix as a WAV,
// prints its level and how long the DSP took, and fails on a silent or
// clipped result. The output is game data: keep it out of the repository.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dol.hpp"
#include "dsp_core.hpp"

namespace {

using mkdd::dsp::Core;

constexpr uint32_t kMemSize = 4u << 20;
std::vector<uint8_t> g_mem(kMemSize);
std::vector<uint8_t> g_aram(mkdd::dsp::kAramSize);
Core g_core;
bool g_irq = false;

[[noreturn]] void fail(const std::string& m) {
    std::fprintf(stderr, "mkdd_dsp_check: %s\n", m.c_str());
    std::exit(1);
}

// Main memory as the DSP's DMA sees it.
uint8_t* memory(void*, uint32_t address, uint32_t bytes) {
    const uint32_t a = address & 0x1FFFFFFFu;
    if (a + bytes > kMemSize) fail("the DSP reached main memory at " + std::to_string(address));
    return g_mem.data() + a;
}
void put16(uint32_t a, uint16_t v) { g_mem[a] = uint8_t(v >> 8), g_mem[a + 1] = uint8_t(v); }
void put32(uint32_t a, uint32_t v) { put16(a, uint16_t(v >> 16)), put16(a + 2, uint16_t(v)); }
int16_t get16(uint32_t a) { return static_cast<int16_t>((g_mem[a] << 8) | g_mem[a + 1]); }

// The CPU's side of the mailboxes.
void send_mail(uint32_t m) {
    if (g_core.cpu_mail & 0x80000000u) fail("the DSP did not take the last mail");
    g_core.cpu_mail = m | 0x80000000u;
    g_core.run();
}
bool mail_waiting() { return (g_core.dsp_mail & 0x80000000u) != 0; }
uint32_t read_mail() {
    if (!mail_waiting()) fail("no mail from the DSP where one was due");
    const uint32_t m = g_core.dsp_mail;
    g_core.dsp_mail &= 0x7FFFFFFFu;
    g_core.run();
    return m;
}
// DSPSendCommands2: the count with the external interrupt, then the words.
void send_command(const std::vector<uint32_t>& words, bool release = false) {
    send_mail(release ? 0 : static_cast<uint32_t>(words.size()));
    g_core.ext_int = true;
    g_core.run();
    for (uint32_t w : words) {
        send_mail(w);
        if (g_core.cpu_mail & 0x80000000u) fail("the DSP left a command word unread");
    }
}

constexpr uint32_t kChannels = 0x100000, kFx = 0x110000, kRes = 0x120000, kAdpcm = 0x121000, kLeft = 0x130000, kRight = 0x140000;
constexpr unsigned kChannelCount = 64, kChannelBytes = 0x180;

struct Wave {
    int osc = 0;  // or a wave:
    bool wave = false;
    unsigned format = 0, samples = 0, loop_start = 0, loop_end = 0;
    bool loop = false;
    uint32_t aram = 0;
};

// JASDsp::TChannel::init, playStart, setWaveInfo or setOscInfo, two mixers to the main buses.
void start_channel(unsigned i, const Wave& w, uint16_t pitch, int16_t volume) {
    const uint32_t c = kChannels + i * kChannelBytes;
    std::memset(g_mem.data() + c, 0, kChannelBytes);
    put16(c + 0x120, 0x7FFF);  // FIR and IIR filters: pass
    put16(c + 0x148, 0x7FFF);
    static const uint8_t block_samples[8] = {16, 16, 1, 1, 1, 16, 16, 1}, block_bytes[8] = {9, 5, 8, 16, 1, 1, 1, 1};
    if (w.wave) {
        put32(c + 0x118, w.aram);
        put16(c + 0x64, block_samples[w.format]);
        put16(c + 0x100, block_bytes[w.format]);
        put32(c + 0x11C, w.samples);
        put16(c + 0x102, w.loop ? 0xFFFF : 0);
        if (w.loop) put32(c + 0x110, w.loop_start), put32(c + 0x114, w.loop_end);
        else put32(c + 0x114, w.samples);
    } else {
        put16(c + 0x64, 16);
        put16(c + 0x100, static_cast<uint16_t>(w.osc));
    }
    put16(c + 0x04, pitch);
    put16(c + 0x08, 1);
    // Mixers: bus address in the DSP, target volume, volume now.
    put16(c + 0x10, 0x0D00), put16(c + 0x12, static_cast<uint16_t>(volume)), put16(c + 0x14, static_cast<uint16_t>(volume));
    put16(c + 0x18, 0x0D60), put16(c + 0x1A, static_cast<uint16_t>(volume)), put16(c + 0x1C, static_cast<uint16_t>(volume));
    put16(c + 0x00, 1);
}
bool channel_active(unsigned i) { return get16(kChannels + i * kChannelBytes) != 0; }
bool channel_finished(unsigned i) { return get16(kChannels + i * kChannelBytes + 2) != 0; }

uint16_t active_map(unsigned group) {
    uint16_t map = 0;
    for (unsigned i = 0; i < 16; ++i) map = static_cast<uint16_t>((map << 1) | (channel_active(group * 16 + i) ? 1 : 0));
    return map;
}
void release_all() {
    for (unsigned g = 0; g < kChannelCount / 16; ++g) send_command({(g << 16) | active_map(g)}, true);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dol_path, wav_path, aw_path;
    unsigned frames = 200, voices = 1, subframes = 7;
    uint32_t aw_offset = 0, aw_bytes = 0, pitch = 0x1000, mixer_level = 0x1000;
    int volume = 0x3000;
    Wave wave;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> unsigned long {
            if (i + 1 >= argc) fail(a + " needs a value");
            return std::strtoul(argv[++i], nullptr, 0);
        };
        if (a == "--wav" && i + 1 < argc) wav_path = argv[++i];
        else if (a == "--aw" && i + 1 < argc) aw_path = argv[++i], wave.wave = true;
        else if (a == "--frames") frames = static_cast<unsigned>(value());
        else if (a == "--voices") voices = static_cast<unsigned>(value());
        else if (a == "--subframes") subframes = static_cast<unsigned>(value());
        else if (a == "--pitch") pitch = static_cast<uint32_t>(value());
        else if (a == "--volume") volume = static_cast<int>(value());
        else if (a == "--level") mixer_level = static_cast<uint32_t>(value());
        else if (a == "--osc") wave.osc = static_cast<int>(value());
        else if (a == "--offset") aw_offset = static_cast<uint32_t>(value());
        else if (a == "--bytes") aw_bytes = static_cast<uint32_t>(value());
        else if (a == "--format") wave.format = static_cast<unsigned>(value());
        else if (a == "--samples") wave.samples = static_cast<unsigned>(value());
        else if (a == "--loop") wave.loop = true, wave.loop_start = static_cast<unsigned>(value()), wave.loop_end = static_cast<unsigned>(value());
        else if (dol_path.empty()) dol_path = a;
        else fail("unexpected argument " + a);
    }
    if (dol_path.empty()) fail("usage: mkdd_dsp_check <main.dol> [--wav out.wav] [--frames n] [--voices n] [--osc n | --aw ...]");
    if (voices > kChannelCount || wave.format > 3) fail("at most 64 voices; formats 0-3");

    auto dol = qilin::Dol::load(dol_path);
    if (!dol) fail(dol.error());
    // JASDSPInterface.cpp's tables in the supported build (GM4E01 rev 0).
    constexpr uint32_t kDolAdpcm = 0x80328E60, kDolRes = 0x80328EA0;
    if (!dol->mapped(kDolAdpcm, 0x540)) fail("this DOL has no data at the filter tables' address: not the supported build");
    std::memcpy(g_mem.data() + kAdpcm, dol->image().data() + (kDolAdpcm - dol->base()), 0x40);
    std::memcpy(g_mem.data() + kRes, dol->image().data() + (kDolRes - dol->base()), 0x500);

    if (wave.wave) {
        FILE* f = std::fopen(aw_path.c_str(), "rb");
        if (!f) fail("can't read " + aw_path);
        wave.aram = 0x10000;
        std::fseek(f, static_cast<long>(aw_offset), SEEK_SET);
        if (aw_bytes == 0 || wave.aram + aw_bytes > g_aram.size() || std::fread(g_aram.data() + wave.aram, 1, aw_bytes, f) != aw_bytes)
            fail("can't read the wave's bytes (--offset, --bytes)");
        std::fclose(f);
    }

    // The microcode: the one program generated, as the boot ROM would start it.
    if (mkdd::dsp::g_program_count == 0) fail("no microcode was lifted into this build");
    const mkdd::dsp::Program* program = &mkdd::dsp::g_programs[0];
    g_core.aram = g_aram.data();
    g_core.memory = memory;
    g_core.irq = [](void*) { g_irq = true; };
    for (const auto& s : dol->sections) {
        if (s.text) continue;
        for (uint32_t a = s.address; a + program->bytes <= s.end(); a += 0x20) {
            const uint8_t* p = dol->image().data() + (a - dol->base());
            if (mkdd::dsp::ector_hash(p, program->bytes) != program->hash) continue;
            for (unsigned i = 0; i < program->bytes / 2; ++i) g_core.iram[i] = static_cast<uint16_t>((p[2 * i] << 8) | p[2 * i + 1]);
            std::printf("microcode %08X (%s) at %08X\n", program->hash, program->name, a);
            goto found;
        }
    }
    fail("the lifted microcode's image is not in this DOL");
found:
    g_core.boot(program, 0);
    g_core.halted = false;
    g_core.run();

    // The handshake: DSP_INIT with the interrupt, then JAudio's own greeting.
    if (!g_irq) fail("the microcode did not interrupt after booting");
    g_irq = false;
    uint32_t m = read_mail();
    if (m != 0xDCD10000u) fail("the first mail is not DSP_INIT");
    m = read_mail();
    if (m != 0xF3551111u) fail("the second mail is not F3551111");
    std::printf("booted: DCD10000, F3551111\n");

    for (unsigned i = 0; i < kChannelCount; ++i) std::memset(g_mem.data() + kChannels + i * kChannelBytes, 0, kChannelBytes);
    // DsetupTable(channels, CH_BUF, DSPRES_FILTER, DSPADPCM_FILTER, FX_BUF)
    send_command({0x81000000u | kChannelCount, 0x80000000u | kChannels, 0x80000000u | kRes, 0x80000000u | kAdpcm, 0x80000000u | kFx});
    if (!g_irq) fail("DsetupTable was not answered");
    g_irq = false;
    m = read_mail();
    const uint32_t ack = read_mail();
    if (m != 0xDCD10004u || ack != 0xF3558100u) fail("DsetupTable's answer is not DCD10004, F3558100");
    std::printf("DsetupTable acknowledged\n");

    for (unsigned v = 0; v < voices; ++v) {
        // Spread the voices' pitches a little so they don't just add up.
        start_channel(v, wave, static_cast<uint16_t>(pitch + v * (pitch / 64)), static_cast<int16_t>(volume / static_cast<int>(voices > 4 ? voices / 4 : 1)));
    }

    std::vector<int16_t> out;
    out.reserve(size_t(frames) * subframes * 160);
    const uint64_t jumps0 = g_core.back_jumps;
    const auto t0 = std::chrono::steady_clock::now();
    unsigned finished_at = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
        // DsyncFrame(subframes, left, right): mixer level in the low half.
        send_command({0x82000000u | (subframes << 16) | mixer_level, 0x80000000u | kLeft, 0x80000000u | kRight});
        for (unsigned sub = 1; sub <= subframes; ++sub) {
            release_all();
            if (!g_irq) fail("no interrupt after a sub-frame's channels were released");
            g_irq = false;
            m = read_mail();
            const uint32_t sync = read_mail();
            if (m != 0xDCD10004u || sync != (0xF355FF00u | sub)) {
                char text[96];
                std::snprintf(text, sizeof text, "after sub-frame %u: %08X, %08X instead of DCD10004, F355FF%02X", sub, m, sync, sub);
                fail(text);
            }
            // The CPU's part between sub-frames: a finished channel is acknowledged and freed.
            for (unsigned i = 0; i < kChannelCount; ++i)
                if (channel_active(i) && channel_finished(i)) {
                    put16(kChannels + i * kChannelBytes + 2, 0);
                    put16(kChannels + i * kChannelBytes, 0);
                    if (!finished_at) finished_at = frame + 1;
                }
        }
        if (!g_irq) fail("no interrupt at the end of the frame");
        g_irq = false;
        m = read_mail();
        if (m != 0xDCD10005u) fail("the frame did not end with DCD10005");
        send_mail(0xCDD10003u);  // carry on
        for (unsigned i = 0; i < subframes * 80; ++i) {
            out.push_back(get16(kLeft + 2 * i));
            out.push_back(get16(kRight + 2 * i));
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double sound = double(frames) * subframes * 80 / 32000.0;

    double energy = 0;
    int peak = 0;
    size_t clipped = 0;
    for (int16_t v : out) {
        energy += double(v) * v;
        peak = std::max(peak, std::abs(int(v)));
        if (v == 32767 || v == -32768) ++clipped;
    }
    const double rms = out.empty() ? 0 : std::sqrt(energy / double(out.size()));
    std::printf("%u frames of %u sub-frames (%.2f s of sound at 32 kHz), %u voices: rms %.1f, peak %d, %zu clipped samples\n", frames,
                subframes, sound, voices, rms, peak, clipped);
    if (finished_at) std::printf("a voice finished in frame %u\n", finished_at);
    std::printf("DSP time %.1f ms: %.2f%% of real time on this machine; %llu backward jumps, %llu ARAM reads\n", seconds * 1000,
                100 * seconds / sound, static_cast<unsigned long long>(g_core.back_jumps - jumps0),
                static_cast<unsigned long long>(g_core.aram_reads));

    if (!wav_path.empty()) {
        FILE* f = std::fopen(wav_path.c_str(), "wb");
        if (!f) fail("can't write " + wav_path);
        const uint32_t bytes = static_cast<uint32_t>(out.size() * 2);
        auto le32 = [&](uint32_t v) { std::fputc(v & 255, f), std::fputc((v >> 8) & 255, f), std::fputc((v >> 16) & 255, f), std::fputc(v >> 24, f); };
        auto le16 = [&](unsigned v) { std::fputc(v & 255, f), std::fputc((v >> 8) & 255, f); };
        std::fwrite("RIFF", 1, 4, f), le32(36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f), le32(16), le16(1), le16(2), le32(32000);
        le32(32000 * 4), le16(4), le16(16), std::fwrite("data", 1, 4, f), le32(bytes);
        for (int16_t v : out) le16(static_cast<uint16_t>(v));
        std::fclose(f);
        std::printf("wrote %s\n", wav_path.c_str());
    }
    if (rms < 1.0) fail("the mix is silent");
    if (clipped > out.size() / 100) fail("more than 1% of the mix is clipped");
    return 0;
}
