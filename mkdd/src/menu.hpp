// The settings menu over the game, as the other Qilin recomps have it: F1 or
// Escape (a controller: Back and Start together) open and close it. It has
// Controls, Display and Game pages; while it's open the game is paused
// (os.hpp), and closing it saves the settings. A hint says how to open it
// for the first seconds of a run.
//
// Controls are set by pressing them: Add on a control's row, then the key,
// the pad's button, stick or trigger, or a click. The picture's settings show
// at once (Look at the picture hides the menu until any key or button); the
// render resolution, the graphics device and the sound's output wait for the
// next start. What needs the game itself patched (enhance.hpp) is shown
// disabled, with what it's waiting for.
//
// F3 switches the frame times, top left. F9 captures the next frames shown
// for a report: the pictures as PNG, the log and the settings, in one tar in
// the data folder's captures/.
//
// Dear ImGui (MIT) with its SDL3 backend for input and its Vulkan backend
// drawing over the picture (display.hpp's overlay). Everything here runs on
// the main thread. Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "qilin/host/display.hpp"
#include "settings.hpp"
#include "save_history.hpp"

namespace mkdd {

class Menu {
public:
    // Without a window there is no menu (ok() is false); the game runs as before.
    Menu(qilin::host::Display& display, Settings& settings, std::filesystem::path folder);
    ~Menu();
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    bool ok() const { return ok_; }
    // Open and showing: the main loop shows the paused picture under it
    // (present_again) and gives the game no input.
    bool is_open() const { return open_; }
    // Once per pass of the main loop, after the display's poll: opens once
    // the game has parked, builds the menu's (or the hint's) drawing.
    void tick();
    // After each new picture the main loop presents: a capture takes it.
    void frame_shown();
    bool quit_requested() const { return quit_; }
    // Whether the controls changed since the last call (the main loop's
    // table of them is rebuilt).
    bool take_controls_changed();
    // The render resolution the game runs at (a change waits for the next start).
    void set_running_scale(unsigned scale) { running_scale_ = scale; }
    // Lines under the frame-time graph (where the time goes), from the main loop.
    void set_frame_details(std::vector<std::string> lines) { frame_details_ = std::move(lines); }
    bool frame_times_shown() const;

private:
    bool handle(const void* sdl_event);
    void toggle();
    void close();
    void save();
    void draw_menu();
    void controls_page();
    void display_page();
    void game_page();
    void draw_hint();
    void draw_frame_times();
    void bind(const Binding& b);
    void take_capture_frame();
    void finish_capture();

    qilin::host::Display& display_;
    Settings& s_;
    std::filesystem::path folder_;
    bool ok_ = false, opening_ = false, open_ = false, quit_ = false, close_asked_ = false;
    bool sdl_backend_ = false, vulkan_backend_ = false;
    bool drawn_ = false, peek_ = false, controls_changed_ = false;
    std::vector<std::string> frame_details_;
    uint64_t hint_until_ = 0;    // SDL ticks (ms)
    std::string banner_;         // said in the hint's place for a while (a capture's file)
    uint64_t banner_until_ = 0;
    // Setting a control by pressing it.
    unsigned player_ = 0;        // whose controls the page shows
    int capture_ = -1;           // the Control being set, or -1
    uint64_t capture_until_ = 0;
    uint32_t capture_held_ = 0;  // the pad's buttons held when it began (they don't count)
    bool wait_release_ = false;  // after a pad button was taken: gamepad navigation back once it's let go
    std::string note_;           // said under the table (what moved where)
    bool history_loaded_ = false;
    std::vector<SaveBackup> save_history_;
    std::vector<std::string> sound_devices_;
    bool sound_devices_loaded_ = false;
    unsigned running_scale_ = 1;
    // F9: the frames still to take, and those taken (PNG).
    unsigned frames_wanted_ = 0;
    std::vector<std::vector<uint8_t>> frames_;
};

}  // namespace mkdd
