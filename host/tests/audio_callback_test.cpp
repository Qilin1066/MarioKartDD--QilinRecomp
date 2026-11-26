// Actual SDL callbacks run concurrently with producers; ctest bounds hangs.
#include "qilin/host/audio.hpp"
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
int main() {
    using qilin::host::AudioOut;
    for (int cycle = 0; cycle < 3; ++cycle) {
        AudioOut out;
        std::vector<int16_t> samples(8192 * 2, 1000);
        std::thread producer([&] {
            for (int i = 0; i < 300; ++i) {
                const unsigned rate = i % 2 ? 48000u : 32000u;
                out.push(samples.data(), 4096, rate);
                SDL_Delay(1);
            }
        });
        std::thread second([&] { for (int i = 0; i < 100; ++i) { out.push(samples.data(), 160, 32000); SDL_Delay(2); } });
        for (unsigned i = 0; i < 200; ++i) { out.set_volume(i % 101, i % 3 == 0); SDL_Delay(1); }
        producer.join(); second.join();
        out.push(samples.data(), 8192, 32000);
        SDL_Delay(60);
        const auto stats = out.take_stats();
        if (stats.callbacks == 0 || stats.dropped == 0 || stats.pushes != 401 || stats.ratio < .995 || stats.ratio > 1.005) {
            std::printf("FAIL callbacks=%u drops=%u pushes=%u ratio=%f\n", stats.callbacks, stats.dropped, stats.pushes, stats.ratio);
            return 1;
        }
        out.push(nullptr, 0, 0);
    }
    std::puts("concurrent callbacks, format changes, volume and shutdown: passed");
    {
        // A device that stops asking for sound (unplugged) is left for the default, while the game pushes on.
        AudioOut out;
        std::vector<int16_t> samples(160 * 2, 1000);
        std::atomic<bool> stop{false};
        std::thread producer([&] {
            while (!stop.load()) {
                out.push(samples.data(), 160, 32000);
                SDL_Delay(5);
            }
        });
        std::string note;
        for (int i = 0; i < 120 && note.empty(); ++i) note = out.keep_playing(), SDL_Delay(10);  // 1.2 s of a live device
        if (!note.empty()) {
            std::printf("FAIL a playing device was taken for lost: %s\n", note.c_str());
            return 1;
        }
        out.stop_device_for_test();
        for (int i = 0; i < 400 && note.empty(); ++i) note = out.keep_playing(), SDL_Delay(10);
        if (note.find("now on") == std::string::npos) {
            std::printf("FAIL a silent device wasn't replaced (%s)\n", note.c_str());
            return 1;
        }
        out.take_stats();
        SDL_Delay(300);
        const auto stats = out.take_stats();
        stop.store(true);
        producer.join();
        if (stats.callbacks == 0 || stats.pushes == 0) {
            std::printf("FAIL the new device doesn't play: callbacks=%u pushes=%u\n", stats.callbacks, stats.pushes);
            return 1;
        }
        std::printf("a lost device: %s; then %u callbacks in 300 ms\n", note.c_str(), stats.callbacks);
    }
    return 0;
}
