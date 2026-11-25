// The window and what's shown in it: SDL3 for the window and its events,
// Vulkan to present. Headless, it draws into an image of its own instead, for
// tests and for machines without a display.
//
// The picture is drawn into the window through a look (plain, or a CRT
// television), at a size the settings choose, after anti-aliasing and
// upscaling where they ask for them (SMAA, FSR 1), and anything drawn over
// it (the settings menu) after. F11 or Alt+Enter switch borderless full screen.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qilin/host/pad.hpp"
#include "qilin/host/frame_times.hpp"
#include "qilin/host/picture.hpp"
#include "qilin/host/vulkan.hpp"

namespace qilin::host {

inline constexpr int kPlayerPlaces = 4;  // the controllers that can each play a port of their own

class Display {
public:
    struct Options {
        const char* title = "Mario Kart: Double Dash!!";
        uint32_t width = 960, height = 720;  // the window's, or the headless target's, size
        bool headless = false;
        bool fullscreen = false;              // borderless, at the desktop's mode
        std::string monitor;                  // the display's name; empty or missing: the main one
        Picture picture;
    };
    explicit Display(const Options& options);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // Handle the window's events. False once the player has closed it.
    bool poll();
    // Every controller SDL lists is opened (the first listed can be a virtual
    // pad or a wheel's base, with the player's real one after it).
    // The buttons held on all of them together and the mouse's left button
    // (PadButton bits), as of the last poll.
    uint32_t buttons() const;
    // Whose controller is whose: four places, each with a controller of its
    // own. A place set to a device's identity (Controller::key) is that
    // device's; one left empty is automatic: the controllers nobody named,
    // in the order each first had a button pressed (a controller nobody
    // touches never joins; one unplugged leaves its place reserved, so the
    // other players do not change places; plugged in again, it has its place
    // back); "none": no controller.
    struct Controller { std::string key, name; uint32_t id; };
    std::vector<Controller> controllers() const;
    void set_player_controllers(const std::array<std::string, kPlayerPlaces>& identities);
    void reset_player_order();
    int joined_pads() const;  // how many have had a button pressed
    bool pad_connected(int place) const;
    std::string player_controller_name(int place) const;
    // The controller at a place, as of the last poll (not connected: at rest).
    PadState pad_state(int place) const;
    bool focused() const;
    // Where a stick's direction and a trigger count as buttons (percent of their travel, 5-95).
    void set_input_thresholds(unsigned stick, unsigned trigger);
    void set_rumble_strength(unsigned percent);
    // A key by its SDL name ("X", "Space", "Left Shift"): a code for key_down,
    // or -1 if there's no such key. Whether it's held, as of the last poll
    // (never, headless).
    int key_code(const char* name) const;
    bool key_down(int code) const;
    // Rumble, on or off: on the controller at a place, or off on every one.
    void rumble_pad(int place, bool on);
    void rumble_off();
    // Show a frame of RGBA8 pixels, at `aspect` (displayed width / height);
    // `lines`: the console picture's height in its own pixels (whole
    // multiples and the CRT's raster go by it). Black around it.
    void present(uint32_t width, uint32_t height, const uint8_t* rgba, float aspect, uint32_t lines);
    // Show an image drawn on this display's GPU (in TRANSFER_SRC_OPTIMAL
    // layout, with sampled usage and a colour view, its work submitted to the
    // same queue before this call), the same way. It's left as it was given.
    void present_image(VkImage image, VkImageView view, uint32_t width, uint32_t height, float aspect, uint32_t lines);
    // Show the last picture again (for a menu over a paused game). Not
    // counted in frame_times().
    void present_again();
    // The intervals between new pictures presented (present and
    // present_image), over the last 20 s (frame_times.hpp).
    const FrameTimes& frame_times() const;
    // Fullscreen's mode, when it isn't the desktop's (or why it couldn't be
    // even): for the log. Empty otherwise.
    const std::string& mode_note() const;
    // Since the last call: frames presented and the time spent waiting to
    // present them (for the previous one to reach the screen, and for a
    // swapchain image): with vsync, mostly the wait for the display.
    struct PresentStats {
        uint32_t presents = 0;
        double wait_ms = 0;  // in all
    };
    PresentStats take_present_stats();
    Gpu& gpu();
    // Headless: the last frame presented, as RGBA8 at the target's size.
    std::vector<uint8_t> read_back();
    // The last picture as it is shown (through the settings' passes and look,
    // without what's drawn over it), as RGBA8 at the window's size (headless:
    // the target's): for a capture. Empty before the first picture.
    std::vector<uint8_t> capture(uint32_t& width, uint32_t& height);
    const std::string& device_name() const;
    // FSR 1 needs compute shaders on the queue that draws.
    bool fsr_available() const;

    // The picture's settings, from the next present.
    void set_picture(const Picture& p);
    const Picture& picture() const;
    bool fullscreen() const;
    void set_fullscreen(bool on);
    // The displays connected, by name, and moving the window to one (empty: the main one).
    std::vector<std::string> monitors() const;
    void move_to(const std::string& monitor);

    // For a UI drawn over the picture: the SDL window (SDL_Window*), the
    // window images' format and count. Null and 0 when headless.
    void* sdl_window() const;
    VkFormat swap_format() const;
    uint32_t swap_image_count() const;
    // Called with every SDL event (an SDL_Event*) before the display's own
    // handling; true: taken, the display ignores it.
    void set_event_hook(std::function<bool(const void* sdl_event)> hook);
    // Called inside each present's drawing (dynamic rendering into the window
    // image, after the picture), to draw over it.
    void set_overlay(std::function<void(VkCommandBuffer)> overlay);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qilin::host
