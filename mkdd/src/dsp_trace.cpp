// Watching the sound path, for development (audio.hpp, docs/audio.md):
//   MKDD_AUDIO_DUMP=<file.wav>  everything the audio DMA plays, as a WAV (kept valid as it grows,
//                               so a run that is killed leaves a file that opens)
//   MKDD_AUDIO_TRACE=1          the log gets the first register traffic (control writes, mails
//                               both ways) and, every 5 s of sound, what the DSP was asked to do:
//                               frames and sub-frames against DMA buffers, the voices' sources,
//                               filters and buses, effect lines, the level of the output, and the
//                               microcode's share of the time.
// Neither costs anything when unset. The dump is game data: keep it out of the repository.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "audio.hpp"
#include "qilin/rt/report.hpp"

namespace mkdd::dsp::trace {
namespace rt = qilin::rt;

namespace {

bool g_on = false;
std::recursive_mutex g_mutex;  // the DSP's thread and the game's both report here
FILE* g_wav = nullptr;
uint64_t g_wav_frames = 0;
unsigned g_wav_rate = 0;
unsigned g_events_left = 120;  // of the first register traffic, logged one by one

struct Window {
    uint64_t frames = 0, clipped = 0, zero_frames = 0;
    double sum[2] = {0, 0}, squares[2] = {0, 0};
    int peak = 0;
    uint64_t dma = 0, frames_done = 0, subframes = 0, commands = 0, releases = 0, switches = 0, voice_subframes = 0;
    std::map<unsigned, uint64_t> source, filter, bus, mail;
    uint64_t looped = 0, auto_mixer = 0, paused = 0, one_pole = 0, fx_on = 0, fx_seen = 0;
    unsigned max_voices = 0, voices_now = 0;
    uint64_t dsp_ns0 = 0, runs0 = 0, wakes0 = 0, settle0 = 0;
} g_w;
uint64_t g_total_frames = 0, g_total_clipped = 0, g_silent_windows = 0, g_windows = 0;

void put32(FILE* f, uint32_t v) { std::fputc(v & 255, f), std::fputc((v >> 8) & 255, f), std::fputc((v >> 16) & 255, f), std::fputc(v >> 24, f); }
void put16(FILE* f, unsigned v) { std::fputc(v & 255, f), std::fputc((v >> 8) & 255, f); }
void wav_header() {
    const uint32_t bytes = static_cast<uint32_t>(g_wav_frames * 4);
    std::fseek(g_wav, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, g_wav), put32(g_wav, 36 + bytes), std::fwrite("WAVEfmt ", 1, 8, g_wav), put32(g_wav, 16), put16(g_wav, 1);
    put16(g_wav, 2), put32(g_wav, g_wav_rate), put32(g_wav, g_wav_rate * 4), put16(g_wav, 4), put16(g_wav, 16);
    std::fwrite("data", 1, 4, g_wav), put32(g_wav, bytes);
    std::fseek(g_wav, 0, SEEK_END);
    std::fflush(g_wav);
}

std::string histogram(const std::map<unsigned, uint64_t>& h, const char* format) {
    std::string out;
    for (const auto& [k, n] : h) {
        char b[48];
        std::snprintf(b, sizeof b, format, k, static_cast<unsigned long long>(n));
        out += b;
    }
    return out.empty() ? " none" : out;
}

void report() {
    const AudioStats& a = audio_stats();
    const double seconds = g_wav_rate ? double(g_w.frames) / g_wav_rate : 0;
    const double n = g_w.frames ? double(g_w.frames) : 1;
    const double dc_l = g_w.sum[0] / n, dc_r = g_w.sum[1] / n;
    const double rms_l = std::sqrt(g_w.squares[0] / n), rms_r = std::sqrt(g_w.squares[1] / n);
    ++g_windows;
    if (g_w.peak == 0) ++g_silent_windows;
    rt::log("audio trace: %.2f s of sound in %llu DMA buffers; frames done %llu, sub-frames %llu, commands %llu, releases %llu, task switches %llu",
            seconds, static_cast<unsigned long long>(g_w.dma), static_cast<unsigned long long>(g_w.frames_done),
            static_cast<unsigned long long>(g_w.subframes), static_cast<unsigned long long>(g_w.commands),
            static_cast<unsigned long long>(g_w.releases), static_cast<unsigned long long>(g_w.switches));
    rt::log("  output: rms %.0f / %.0f, DC %.1f / %.1f, peak %d, clipped %llu, all-zero frames %llu of %llu", rms_l, rms_r, dc_l, dc_r, g_w.peak,
            static_cast<unsigned long long>(g_w.clipped), static_cast<unsigned long long>(g_w.zero_frames),
            static_cast<unsigned long long>(g_w.frames));
    rt::log("  voices: %.1f a sub-frame on average, %u at most; sources (code:sub-frames)%s", g_w.subframes ? double(g_w.voice_subframes) / double(g_w.subframes) : 0.0,
            g_w.max_voices, histogram(g_w.source, " %u:%llu").c_str());
    rt::log("  filter modes%s; looping %llu, auto-mixer %llu, paused %llu, one-pole %llu; buses%s", histogram(g_w.filter, " %02X:%llu").c_str(),
            static_cast<unsigned long long>(g_w.looped), static_cast<unsigned long long>(g_w.auto_mixer), static_cast<unsigned long long>(g_w.paused),
            static_cast<unsigned long long>(g_w.one_pole), histogram(g_w.bus, " %04X:%llu").c_str());
    rt::log("  effect lines on: %llu of %llu looked at; mails from the DSP%s", static_cast<unsigned long long>(g_w.fx_on),
            static_cast<unsigned long long>(g_w.fx_seen), histogram(g_w.mail, " %08X:%llu").c_str());
    const double dsp_ms = double(a.dsp_ns - g_w.dsp_ns0) / 1e6;
    rt::log("  the microcode: %.1f ms in %llu runs, %.2f%% of the sound's time", dsp_ms, static_cast<unsigned long long>(a.dsp_runs - g_w.runs0),
            seconds > 0 ? dsp_ms / 10.0 / seconds : 0.0);
    if (a.wakes || a.settle_ns)
        rt::log("  the DSP's thread: woken from sleep %llu times; the game's thread waited %.1f ms for it, %.2f ms at worst",
                static_cast<unsigned long long>(a.wakes - g_w.wakes0), double(a.settle_ns - g_w.settle0) / 1e6, double(a.settle_worst_ns) / 1e6);
    const uint64_t wakes = a.wakes, settle = a.settle_ns;
    g_w = Window{};
    g_w.dsp_ns0 = a.dsp_ns;
    g_w.runs0 = a.dsp_runs;
    g_w.wakes0 = wakes;
    g_w.settle0 = settle;
}

}  // namespace

bool init() {
    if (const char* path = std::getenv("MKDD_AUDIO_DUMP"); path && *path) {
        g_wav = std::fopen(path, "wb");
        if (!g_wav) rt::fatal(nullptr, "MKDD_AUDIO_DUMP: can't write %s", path);
    }
    g_on = std::getenv("MKDD_AUDIO_TRACE") != nullptr;
    return g_on || g_wav;
}

void event(const char* what, uint32_t value) {
    const std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_on || !g_events_left) return;
    --g_events_left;
    rt::log("audio trace: %s %08X", what, value);
}

void samples(const int16_t* lr, size_t frames, unsigned rate) {
    const std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_wav) {
        if (g_wav_rate && rate != g_wav_rate) rt::log("audio trace: the rate changed from %u to %u Hz: the dump keeps the first", g_wav_rate, rate);
        if (!g_wav_rate) g_wav_rate = rate, wav_header();
        for (size_t i = 0; i < frames * 2; ++i) put16(g_wav, static_cast<uint16_t>(lr[i]));
        g_wav_frames += frames;
    }
    if (!g_wav_rate) g_wav_rate = rate;
    ++g_w.dma;
    for (size_t i = 0; i < frames; ++i) {
        const int l = lr[2 * i], r = lr[2 * i + 1];
        g_w.sum[0] += l, g_w.sum[1] += r;
        g_w.squares[0] += double(l) * l, g_w.squares[1] += double(r) * r;
        g_w.peak = std::max(g_w.peak, std::max(std::abs(l), std::abs(r)));
        if (l == 32767 || l == -32768 || r == 32767 || r == -32768) ++g_w.clipped, ++g_total_clipped;
        if (l == 0 && r == 0) ++g_w.zero_frames;
    }
    g_w.frames += frames;
    g_total_frames += frames;
    if (g_w.frames >= uint64_t(rate) * 5) {
        if (g_wav) wav_header();
        if (g_on) report();
        else g_w = Window{};
    }
}

// A DMA between main memory and the DSP's data RAM: channel blocks and effect lines on their way in.
void dma(unsigned control, unsigned at, unsigned words, const uint8_t* p) {
    const std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_on || (control & 1)) return;
    const auto word = [&](unsigned byte) { return static_cast<unsigned>((p[byte] << 8) | p[byte + 1]); };
    if (at == 0x0400 && words == 0xC0) {  // JASDsp::TChannel
        if (!word(0x00) || word(0x02)) return;  // inactive or finished: not processed
        ++g_w.voice_subframes;
        ++g_w.voices_now;
        ++g_w.source[word(0x100)];
        ++g_w.filter[word(0x108)];
        if (word(0x102)) ++g_w.looped;
        if (word(0x58)) ++g_w.auto_mixer;
        if (word(0x0C)) ++g_w.paused;
        if (word(0x150)) ++g_w.one_pole;
        for (unsigned m = 0; m < 6; ++m)
            if (word(0x10 + 8 * m)) ++g_w.bus[word(0x10 + 8 * m)];
    } else if (at == 0x03A8 && words == 0x40) {  // four JASDsp::Fxline
        for (unsigned i = 0; i < 4; ++i) {
            ++g_w.fx_seen;
            if (word(0x20 * i)) ++g_w.fx_on;
        }
    }
}

void mail_from_dsp(uint32_t mail) {
    const std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_on) return;
    event("the CPU read mail", mail);
    if ((mail >> 16) == 0xF355 && (mail & 0xFF00) == 0xFF00) {
        ++g_w.subframes;
        g_w.max_voices = std::max(g_w.max_voices, g_w.voices_now);
        g_w.voices_now = 0;
        return;  // (counted; not one histogram entry per number)
    }
    if (mail == 0xDCD10005u) ++g_w.frames_done;
    ++g_w.mail[mail];
}
void mail_to_dsp(uint32_t mail, bool lifted) {
    const std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_on) return;
    event(lifted ? "mail to the microcode" : "mail to the ROM", mail);
    if (lifted && mail == 0xCDD10001u) ++g_w.switches;
}
void interrupt_dsp(uint32_t waiting_mail) {
    const std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_on) return;
    if ((waiting_mail & 0xFFFF) == 0) ++g_w.releases;
    else ++g_w.commands;
}

}  // namespace mkdd::dsp::trace
