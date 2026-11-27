#include "qilin/host/audio.hpp"
#include "qilin/host/vulkan.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace qilin::host {

namespace {
// Through the host's fatal path (vulkan.hpp's die), so the message reaches the log.
[[noreturn]] void fail(const char* what) { die("sound: %s: %s", what, SDL_GetError()); }
// SDL holds this same recursive lock while calling feed(). All producers
// take it first as well: there is no second mutex whose order can invert.
struct StreamLock {
    SDL_AudioStream* stream;
    explicit StreamLock(SDL_AudioStream* s) : stream(s) { if (!SDL_LockAudioStream(stream)) fail("sound lock failed"); }
    ~StreamLock() { SDL_UnlockAudioStream(stream); }
};
size_t frames_of(double ms, unsigned rate) { return static_cast<size_t>(ms * double(rate) / 1000.0); }
}  // namespace

std::vector<std::string> AudioOut::playback_devices() {
    std::vector<std::string> out;
    const bool started = SDL_InitSubSystem(SDL_INIT_AUDIO);
    int n = 0;
    if (SDL_AudioDeviceID* ids = started ? SDL_GetAudioPlaybackDevices(&n) : nullptr) {
        for (int i = 0; i < n; ++i)
            if (const char* name = SDL_GetAudioDeviceName(ids[i])) out.push_back(name);
        SDL_free(ids);
    }
    if (started) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return out;
}

AudioOut::AudioOut() : AudioOut(Options{}) {}

AudioOut::AudioOut(const Options& options) : options_(options) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) fail("SDL audio can't start");
    rate_ = 32000;
    if (!open(false)) fail("no sound output device");
}

// The stream on the device the options name (or, `fallback`, the system's
// default whatever they name), at the current rate. Under device_lock_ once
// running.
bool AudioOut::open(bool fallback) {
    // The device asked for, if it's there; else the system's default, which SDL
    // moves to the new default device when the system's changes.
    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    device_name_ = "the system's default device";
    if (!options_.device.empty() && !fallback) {
        int n = 0;
        if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n)) {
            for (int i = 0; i < n && id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK; ++i)
                if (const char* name = SDL_GetAudioDeviceName(ids[i]); name && options_.device == name) id = ids[i], device_name_ = name;
            SDL_free(ids);
        }
        if (id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)
            std::fprintf(stderr, "mkdd: the settings' sound device \"%s\" isn't connected: using the system's default\n", options_.device.c_str());
    }
    // Surround where it's asked for, or where it's left to a device that has six channels or more.
    channels_ = options_.output == SoundOutput::surround ? 6 : 2;
    if (options_.output == SoundOutput::device) {
        SDL_AudioSpec own{};
        if (SDL_GetAudioDeviceFormat(id, &own, nullptr) && own.channels >= 6) channels_ = 6;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, static_cast<int>(channels_), static_cast<int>(rate_)};
    surround_.set_rate(rate_);
    transitions_.set_rate(rate_);
    discontinuity_ = false;
    // The device asks for sound as it needs it (feed): nothing waits inside SDL.
    stream_ = SDL_OpenAudioDeviceStream(id, &spec, [](void* self, SDL_AudioStream* stream, int additional, int) {
        static_cast<AudioOut*>(self)->feed(stream, additional);
    }, this);
    if (!stream_) return false;
    // A device that takes its sound in large pieces needs more queued than one that takes 10 ms at a time.
    SDL_AudioSpec device{};
    int piece = 0;
    device_period_ms_ = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &piece) && device.freq > 0 && piece > 0)
        device_period_ms_ = double(piece) * 1000.0 / double(device.freq);
    target_ms_ = std::max(kTargetMs, 2.0 * device_period_ms_ + 10.0);
    smoothed_ms_ = target_ms_;
    ratio_ = 1.0;
    ratio_ppm_.store(1000000, std::memory_order_relaxed);
    queue_.clear();
    queue_.set_limits(frames_of(target_ms_, rate_), frames_of(2.0 * target_ms_, rate_));
    if (gain_ != 1.0f) SDL_SetAudioStreamGain(stream_, gain_);
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        return false;
    }
    return true;
}

AudioOut::~AudioOut() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

// The main thread, each pass: a device that has stopped asking for sound (its
// callback silent for a second while the program ran) was unplugged or lost
// (a Bluetooth headset gone, a USB card pulled, the driver restarted): SDL
// moves a stream on the default device by itself, but one on a device chosen
// by name just goes quiet. Here it moves to the system's default, and with no
// device at all it tries again every few seconds; nothing stops the game.
std::string AudioOut::keep_playing() {
    const uint64_t now = SDL_GetTicksNS();
    if (now - checked_ns_ < 500'000'000) return {};
    const uint64_t since = now - checked_ns_;
    checked_ns_ = now;
    const uint64_t feeds = feeds_.load(std::memory_order_relaxed);
    const bool moved = feeds != feeds_seen_;
    feeds_seen_ = feeds;
    // A long time since the last look: this thread didn't run either (the PC asleep): not the device's fault.
    if (moved || since > 2'000'000'000) {
        silent_ = 0;
        return {};
    }
    if (stream_ && ++silent_ < 2) return {};
    if (!stream_ && now - retry_ns_ < 3'000'000'000) return {};
    retry_ns_ = now;
    const std::string was = device_name_;
    const std::lock_guard l(device_lock_);
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    silent_ = 0;
    if (!open(true)) {
        device_name_ = "none";
        return was == "none" ? std::string() : "sound: " + was + " stopped, and there is no other sound device: silent until one is connected";
    }
    return "sound: " + was + " stopped (unplugged?): now on " + device_name_ + (surround() ? ", 5.1" : ", stereo");
}

void AudioOut::stop_device_for_test() {
    const std::lock_guard l(device_lock_);
    if (stream_) SDL_PauseAudioStreamDevice(stream_);
}

// The device's thread: SDL needs `bytes` more of the stream's own format.
void AudioOut::feed(SDL_AudioStream* stream, int bytes) {
    feeds_.fetch_add(1, std::memory_order_relaxed);  // (alive: keep_playing)
    if (bytes <= 0) return;
    const size_t frame_bytes = size_t{2} * channels_;
    const size_t frames = (static_cast<size_t>(bytes) + frame_bytes - 1) / frame_bytes;
    feeding_.resize(frames * 2);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    bool ran_dry;
    const size_t n = queue_.pull(feeding_.data(), frames, ran_dry);
    if (ran_dry) empty_.fetch_add(1, std::memory_order_relaxed);
    transitions_.process(feeding_.data(), n, frames, discontinuity_);
    discontinuity_ = false;
    const int16_t* data = feeding_.data();
    if (channels_ == 6) {
        decoded_.resize(frames * 6);
        surround_.decode(feeding_.data(), frames, decoded_.data());
        data = decoded_.data();
    }
    if (!SDL_PutAudioStreamData(stream, data, static_cast<int>(frames * frame_bytes))) fail("sound callback cannot queue data");
}

void AudioOut::push(const int16_t* lr, size_t frames, unsigned rate) {
    if (!frames) return;
    if (!lr || rate == 0 || rate > 192000 || frames > size_t(std::numeric_limits<int>::max()) / 4)
        fail("invalid sound buffer or sample rate");
    const std::lock_guard device_lock(device_lock_);  // (before the stream's: keep_playing takes them in this order)
    if (!stream_) return;                              // no device for now: the sound goes nowhere
    const StreamLock stream_lock(stream_);
    size_t before, dropped;
    {
        if (rate != rate_) {  // what is queued was made for the old rate: it goes
            if (!SDL_ClearAudioStream(stream_)) fail("old sound format cannot be cleared");
            smoothed_ms_ = target_ms_;
            const SDL_AudioSpec spec{SDL_AUDIO_S16, static_cast<int>(channels_), static_cast<int>(rate)};
            if (!SDL_SetAudioStreamFormat(stream_, &spec, nullptr)) fail("the sound rate can't change");
            rate_ = rate;
            surround_.set_rate(rate_);
            transitions_.set_rate(rate_);
            discontinuity_ = false;
            queue_.clear();
            queue_.set_limits(frames_of(target_ms_, rate_), frames_of(2.0 * target_ms_, rate_));
        }
        before = queue_.size();
        dropped = queue_.push(lr, frames);
    }
    if (dropped) {
        discontinuity_ = true;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        dropped_us_.fetch_add(static_cast<uint64_t>(double(dropped) * 1e6 / double(rate)), std::memory_order_relaxed);
        smoothed_ms_ = target_ms_;
    }
    const double ms = double(before) * 1000.0 / double(rate);
    // Smoothed over about 50 pushes (a quarter second at 5 ms each): the drift is slow.
    smoothed_ms_ += ((dropped ? target_ms_ : ms) - smoothed_ms_) * 0.02;
    const double ratio = audio_rate_ratio(smoothed_ms_, target_ms_);
    if (std::fabs(ratio - ratio_) >= 0.0001) {  // SDL rebuilds its resampler: not for every push
        if (!SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(ratio))) fail("the sound speed can't change");
        ratio_ = ratio;
        ratio_ppm_.store(static_cast<uint32_t>(std::lround(ratio * 1e6)), std::memory_order_relaxed);
    }
    const auto us = static_cast<uint32_t>(ms * 1000.0);
    sum_us_.fetch_add(us, std::memory_order_relaxed);
    pushes_.fetch_add(1, std::memory_order_relaxed);
    uint32_t m = max_us_.load(std::memory_order_relaxed);
    while (us > m && !max_us_.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
}

void AudioOut::set_volume(unsigned percent, bool muted) {
    const std::lock_guard device_lock(device_lock_);
    const float gain = muted ? 0.0f : float(std::min(percent, 100u)) / 100.0f;
    if (gain == gain_ || !stream_) {
        gain_ = gain;  // (a device opened later takes it)
        return;
    }
    const StreamLock stream_lock(stream_);
    if (!SDL_SetAudioStreamGain(stream_, gain)) fail("sound volume cannot change");
    gain_ = gain;
}

AudioOut::Stats AudioOut::take_stats() {
    Stats s;
    s.pushes = pushes_.exchange(0, std::memory_order_relaxed);
    const uint64_t sum = sum_us_.exchange(0, std::memory_order_relaxed);
    s.average_ms = s.pushes ? double(sum) / double(s.pushes) / 1000.0 : 0;
    s.max_ms = max_us_.exchange(0, std::memory_order_relaxed) / 1000.0;
    s.empty = empty_.exchange(0, std::memory_order_relaxed);
    s.dropped = dropped_.exchange(0, std::memory_order_relaxed);
    s.callbacks = callbacks_.exchange(0, std::memory_order_relaxed);
    s.dropped_ms = double(dropped_us_.exchange(0, std::memory_order_relaxed)) / 1000.0;
    s.ratio = ratio_ppm_.load(std::memory_order_relaxed) / 1e6;
    return s;
}

}  // namespace qilin::host
