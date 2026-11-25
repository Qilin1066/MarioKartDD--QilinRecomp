// The settings menu (menu.hpp). Runtime code: no std::format.
#include "menu.hpp"

#include <cmath>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "enhance.hpp"
#include "os.hpp"
#include "save_history.hpp"
#include "qilin/host/capture.hpp"
#include "qilin/host/pacing.hpp"
#include "qilin/rt/system.hpp"

#ifndef MKDD_VERSION
#define MKDD_VERSION "?"
#endif

namespace mkdd {
namespace host = qilin::host;

namespace {

constexpr uint64_t kHintMs = 10000;    // how long the hint shows at the start
constexpr uint64_t kBannerMs = 6000;   // and a capture's file name
constexpr uint64_t kCaptureMs = 6000;  // how long a control waits to be pressed
constexpr unsigned kCaptureFrames = 4; // F9: this many pictures in a row
constexpr const char* kHint = "F1 or Esc (controller: Back + Start): settings";
// What an option that needs the game patched says beside it (enhance.hpp).
constexpr const char* kNeedsAspect = "(not yet: needs the game's cameras, culling and HUD patched; the picture is 4:3)";
constexpr const char* kNeedsFrameRate = "(not yet: needs the game's frame step patched; it runs at its own 60, or 30 with three or four players)";

// ImGui's Vulkan backend loads its functions through volk's loader.
VkInstance g_instance = VK_NULL_HANDLE;
PFN_vkVoidFunction load(const char* name, void*) { return vkGetInstanceProcAddr(g_instance, name); }
void check(VkResult r) {
    if (r < 0) qilin::rt::log("settings menu: Vulkan error %d", int(r));
}

uint32_t lowest_bit(uint32_t v) { return v & (~v + 1); }

}  // namespace

Menu::Menu(host::Display& display, Settings& settings, std::filesystem::path folder)
    : display_(display), s_(settings), folder_(std::move(folder)) {
    auto* window = static_cast<SDL_Window*>(display.sdl_window());
    if (!window) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the settings file is the only file
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    // 1.25x, as the other recomps have it, times the display's own scale.
    float dpi = SDL_GetWindowDisplayScale(window);
    if (!(dpi > 0.0f)) dpi = 1.0f;
    ImGui::GetStyle().ScaleAllSizes(1.25f * dpi);
    ImGui::GetStyle().FontScaleMain = 1.25f;
    ImGui::GetStyle().FontScaleDpi = dpi;

    sdl_backend_ = ImGui_ImplSDL3_InitForVulkan(window);
    // Every controller works the menu, not only the first one SDL lists
    // (which can be a virtual pad, or a wheel's base).
    if (sdl_backend_) ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    host::Gpu& gpu = display.gpu();
    g_instance = gpu.instance;
    if (!sdl_backend_ || !ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load)) {
        qilin::rt::log("the settings menu can't start (its SDL or Vulkan side)");
        return;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = gpu.instance;
    ii.PhysicalDevice = gpu.physical;
    ii.Device = gpu.device;
    ii.QueueFamily = gpu.family;
    ii.Queue = gpu.queue;
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
    ii.MinImageCount = std::max(2u, display.swap_image_count());
    ii.ImageCount = ii.MinImageCount;
    ii.UseDynamicRendering = true;
    static VkFormat format;  // the backend keeps a pointer to it
    format = display.swap_format();
    VkPipelineRenderingCreateInfoKHR& pr = ii.PipelineInfoMain.PipelineRenderingCreateInfo;
    pr = {};
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    pr.colorAttachmentCount = 1, pr.pColorAttachmentFormats = &format;
    ii.CheckVkResultFn = check;
    vulkan_backend_ = ImGui_ImplVulkan_Init(&ii);
    if (!vulkan_backend_) {
        qilin::rt::log("the settings menu can't start (its Vulkan side)");
        return;
    }
    display.set_overlay([this](VkCommandBuffer cb) {
        if (drawn_) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    });
    display.set_event_hook([this](const void* e) { return handle(e); });
    ok_ = true;
    hint_until_ = SDL_GetTicks() + kHintMs;
    qilin::rt::log("%s", kHint);
}

Menu::~Menu() {
    display_.set_overlay(nullptr);
    display_.set_event_hook(nullptr);
    if (vulkan_backend_) {
        vkDeviceWaitIdle(display_.gpu().device);
        ImGui_ImplVulkan_Shutdown();
    }
    if (sdl_backend_) ImGui_ImplSDL3_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

bool Menu::take_controls_changed() {
    const bool was = controls_changed_;
    controls_changed_ = false;
    return was;
}

void Menu::save() {
    s_.fullscreen = display_.fullscreen();
    if (!save_settings(folder_, s_)) qilin::rt::log("the settings can't be saved to %s", settings_file(folder_).string().c_str());
}

bool Menu::handle(const void* event) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(event);
    if (!ok_) return false;
    if (!display_.focused() && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP ||
        e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
        e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL ||
        e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
        e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION || e.type == SDL_EVENT_TEXT_INPUT)) return false;
    // Looking at the picture: any key or button brings the menu back.
    if (peek_ && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) {
        peek_ = false;
        return true;
    }
    // Setting a control: the key pressed (pad inputs and clicks are read in tick).
    if (capture_ >= 0 && e.type == SDL_EVENT_KEY_DOWN) {
        if (!e.key.repeat) {
            const char* name = SDL_GetScancodeName(e.key.scancode);
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                capture_ = -1, wait_release_ = true, note_.clear();
            } else if (name && *name && !key_reserved(name)) {
                bind({Binding::kKey, 0, name});
            }
        }
        return true;
    }
    const bool shown = open_ || opening_;
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat && (e.key.key == SDLK_F1 || e.key.key == SDLK_ESCAPE)) {
                toggle();
                return true;
            }
            if (!e.key.repeat && e.key.key == SDLK_F3) {  // the frame-time readout, kept in the settings
                s_.performance_overlay = !s_.performance_overlay;
                if (!save_settings(folder_, s_))
                    qilin::rt::log("the settings can't be saved to %s", settings_file(folder_).string().c_str());
                return true;
            }
            if (!e.key.repeat && e.key.key == SDLK_F9) {  // a capture of the next frames, for a report
                if (!frames_wanted_) frames_wanted_ = kCaptureFrames, frames_.clear();
                return true;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            if (capture_ >= 0) break;
            // Back and Start together (alone, Start is the game's); with the
            // menu open, Start alone closes it too.
            SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
            const bool back = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
            const bool start = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
            if ((back && start) || (open_ && e.gbutton.button == SDL_GAMEPAD_BUTTON_START)) {
                toggle();
                return true;
            }
            break;
        }
        default: break;
    }
    if (open_) ImGui_ImplSDL3_ProcessEvent(&e);
    if (!shown) return false;
    switch (e.type) {  // the game doesn't see input while the menu is up
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: return true;
        default: return false;
    }
}

void Menu::toggle() {
    if (open_ || opening_) {
        close();
        return;
    }
    history_loaded_ = false;
    opening_ = true;
    request_pause(true);  // the game parks at its next retrace; tick() opens then
}

void Menu::close() {
    if (open_) save();
    open_ = opening_ = peek_ = false;
    capture_ = -1;
    drawn_ = false;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags = (io.ConfigFlags | ImGuiConfigFlags_NavEnableGamepad) & ~ImGuiConfigFlags_NoMouse;
    wait_release_ = false;
    request_pause(false);
}

// An input presses one control: a key or the mouse one in all (the keyboard
// is everybody's), a pad's input one of its player's.
void Menu::bind(const Binding& b) {
    note_.clear();
    for (unsigned p = 0; p < kPlayers; ++p) {
        if (b.kind == Binding::kPad && p != player_) continue;
        for (unsigned c = 0; c < kControlCount; ++c) {
            auto& list = s_.players[p].controls[c];
            const auto it = std::find(list.begin(), list.end(), b);
            if (it == list.end()) continue;
            if (p != player_ || int(c) != capture_) {
                note_ = binding_label(b) + " moved here from " + control_label(Control(c));
                if (p != player_) note_ += " (player " + std::to_string(p + 1) + ")";
                note_ += ".";
            }
            list.erase(it);
        }
    }
    s_.players[player_].controls[capture_].push_back(b);
    controls_changed_ = true;
    capture_ = -1;
    wait_release_ = true;
}

void Menu::tick() {
    if (!ok_) return;
    if (opening_ && game_parked()) {
        opening_ = false, open_ = true, hint_until_ = 0;
        ImGui::GetIO().ClearInputKeys();
    }
    ImGuiIO& io = ImGui::GetIO();
    if (open_ && capture_ >= 0) {
        // A pad's button, stick or trigger, or a click, pressed since it began sets the control.
        const uint32_t held = display_.buttons();
        const uint32_t fresh = held & ~capture_held_;
        capture_held_ &= held;  // let go, a button counts again
        if (fresh) {
            const uint32_t bit = lowest_bit(fresh);
            bind(bit == host::kPadClick ? Binding{Binding::kMouse, 0, {}} : Binding{Binding::kPad, bit, {}});
        } else if (SDL_GetTicks() >= capture_until_) {
            capture_ = -1, wait_release_ = true;
            note_ = "Nothing was pressed: unchanged.";
        }
    }
    // The mouse and the pad's navigation come back once the press that set a control is let go.
    if (wait_release_ && display_.buttons() == 0) {
        io.ConfigFlags = (io.ConfigFlags | ImGuiConfigFlags_NavEnableGamepad) & ~ImGuiConfigFlags_NoMouse;
        wait_release_ = false;
    }
    // A capture asked for while the game stands still: the picture under the menu, once.
    if (open_ && frames_wanted_) {
        take_capture_frame();
        frames_wanted_ = 0;
        finish_capture();
    }
    if (open_) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (!peek_) draw_menu();
        ImGui::Render();
        drawn_ = true;
        display_.set_picture(picture_of(s_));  // shown at once, under the menu
        if (close_asked_ || quit_) {
            close_asked_ = false;
            if (quit_) save();
            else close();
        }
        return;
    }
    const bool hint = SDL_GetTicks() < hint_until_ || SDL_GetTicks() < banner_until_;
    if (hint || s_.performance_overlay) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (hint) draw_hint();
        if (s_.performance_overlay) draw_frame_times();
        ImGui::Render();
        drawn_ = true;
        return;
    }
    drawn_ = false;
}

bool Menu::frame_times_shown() const { return s_.performance_overlay; }

void Menu::frame_shown() {
    if (!ok_ || !frames_wanted_) return;
    take_capture_frame();
    if (--frames_wanted_ == 0) finish_capture();
}

void Menu::take_capture_frame() {
    uint32_t w = 0, h = 0;
    const std::vector<uint8_t> rgba = display_.capture(w, h);
    if (!rgba.empty()) frames_.push_back(host::png_rgb(rgba.data(), w, h));
}

// captures/capture-<date>-<time>.tar in the data folder: the pictures, the
// log so far, the settings, and what the program knows of the machine.
void Menu::finish_capture() {
    std::fflush(nullptr);  // the log, as far as it has got
    char stamp[32] = "unknown";
    const std::time_t now = std::time(nullptr);
    if (const std::tm* local = std::localtime(&now)) std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", local);
    const std::filesystem::path dir = folder_ / "captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string name = std::string("capture-") + stamp;
    const std::filesystem::path file = dir / (name + ".tar");
    host::TarWriter tar(file);
    for (size_t i = 0; i < frames_.size(); ++i)
        tar.add(name + "/frame_" + std::to_string(i + 1) + ".png", frames_[i].data(), frames_[i].size());
    const std::string settings = settings_text(s_);
    tar.add(name + "/mkdd.ini", settings.data(), settings.size());
    tar.add_file(name + "/mkdd.log", folder_ / "mkdd.log");  // (a run with MKDD_SAVE_DIR elsewhere has none here: left out)
    std::string info = std::string("Mario Kart: Double Dash!! QilinRecomp ") + MKDD_VERSION + "\n";
    info += "graphics device: " + display_.device_name() + "\n";
    info += "render resolution: " + std::to_string(running_scale_) + "x\n";
    char line[160];
    std::snprintf(line, sizeof line, "display refresh: %.3f Hz\n", g_display_hz.load(std::memory_order_relaxed));
    info += line;
    const host::FrameTimes::Summary ft = display_.frame_times().summary();
    std::snprintf(line, sizeof line, "frame times: %.1f fps (%.2f ms), 1%% low %.1f, 0.1%% low %.1f, worst %.1f ms over %u frames\n", ft.fps,
                  ft.average_ms, ft.low1_fps, ft.low01_fps, ft.worst_ms, unsigned(ft.frames));
    info += line;
    for (const std::string& detail : frame_details_) info += detail + "\n";
    info += "frames captured: " + std::to_string(frames_.size()) + (open_ ? " (the settings menu was open: the paused picture)\n" : "\n");
    tar.add(name + "/info.txt", info.data(), info.size());
    const bool ok = tar.close();
    frames_.clear();
    banner_ = ok ? "Captured for a report: " + file.string() : "The capture couldn't be written to " + file.string();
    banner_until_ = SDL_GetTicks() + kBannerMs;
    qilin::rt::log("%s", banner_.c_str());
}

// F3: how evenly frames reach the screen, top left. The lows are the frame
// rates of the slowest 1% and 0.1% of frames over the last 20 s.
void Menu::draw_frame_times() {
    const host::FrameTimes& t = display_.frame_times();
    const host::FrameTimes::Summary s = t.summary();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8.0f, vp->WorkPos.y + 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##frame times", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (s.frames == 0) {
        ImGui::TextUnformatted("frame times: waiting for frames");
    } else {
        ImGui::Text("%.1f fps  %.2f ms", s.fps, s.average_ms);
        ImGui::Text("1%% low %.1f  0.1%% low %.1f", s.low1_fps, s.low01_fps);
        ImGui::Text("worst %.1f ms", s.worst_ms);
        // The game draws 59.94 frames a second, as on the console (half that
        // with three or four players); a faster display shows each one more
        // than once, with less wait before it's seen.
        const double hz = mkdd::g_display_hz.load(std::memory_order_relaxed);
        if (hz > 0) {
            const host::RetraceRate r = host::retrace_rate(uint32_t(std::lround(hz * 1000.0)), 1000, s_.vsync, s_.variable_refresh);
            if (r.locked && r.every >= 2) ImGui::Text("display %.0f Hz: each frame shown %u times", hz, r.every);
            else if (!r.locked && s_.vsync && !s_.variable_refresh) ImGui::Text("display %.1f Hz: not a multiple of 59.94 (uneven)", hz);
            else ImGui::Text("display %.0f Hz", hz);
        }
        const std::vector<float> recent = t.recent(240);
        ImGui::PlotLines("##ft", recent.data(), int(recent.size()), 0, nullptr, 0.0f, 50.0f, ImVec2(240.0f, 48.0f));
    }
    for (const std::string& line : frame_details_) ImGui::TextUnformatted(line.c_str());
    ImGui::End();
}

void Menu::draw_hint() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 12.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, vp->WorkPos.y + vp->WorkSize.y - pad), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(SDL_GetTicks() < banner_until_ ? banner_.c_str() : kHint);
    ImGui::End();
}

void Menu::draw_menu() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Mario Kart: Double Dash!! - settings (the game is paused)");
    // Close and Quit, top right on every page, as in the other recomps.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Close (Esc)").x + ImGui::CalcTextSize("Quit the game").x + 4 * st.FramePadding.x + st.ItemSpacing.x;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttons + ImGui::GetCursorPosX());
    if (ImGui::Button("Close (Esc)")) close_asked_ = true;  // tick() closes it, once the frame is built
    ImGui::SameLine();
    if (ImGui::Button("Quit the game")) quit_ = true;
    ImGui::Separator();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    if (ImGui::BeginTabBar("pages")) {
        if (ImGui::BeginTabItem("Controls")) {
            controls_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            display_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            game_page();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    if (!banner_.empty() && SDL_GetTicks() < banner_until_) ImGui::TextWrapped("%s", banner_.c_str());
    ImGui::End();
}

void Menu::controls_page() {
    ImGui::SeparatorText("Controllers");
    const auto devices = display_.controllers();
    if (devices.empty()) ImGui::TextDisabled("None connected: the keyboard plays the first controller port.");
    for (const auto& device : devices) ImGui::BulletText("%s", device.name.c_str());
    // The four ports: who plays each.
    if (ImGui::BeginTable("ports", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthStretch, 0.12f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch, 0.33f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        for (unsigned p = 0; p < kPlayers; ++p) {
            Player& pl = s_.players[p];
            ImGui::PushID(int(p));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Player %u", p + 1);
            ImGui::TableNextColumn();
            std::string preview = pl.controller.empty() ? "Automatic" : pl.controller == "none" ? "None" : "Saved controller (disconnected)";
            for (const auto& device : devices)
                if (device.key == pl.controller) preview = device.name;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##controller", preview.c_str())) {
                if (ImGui::Selectable("Automatic", pl.controller.empty())) pl.controller.clear();
                if (ImGui::Selectable("None", pl.controller == "none")) pl.controller = "none";
                for (const auto& device : devices) {
                    bool taken = false;  // somebody else's by name
                    for (unsigned o = 0; o < kPlayers; ++o) taken = taken || (o != p && s_.players[o].controller == device.key);
                    ImGui::PushID(int(device.id));
                    ImGui::BeginDisabled(taken);
                    if (ImGui::Selectable(device.name.c_str(), pl.controller == device.key)) pl.controller = device.key;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            bool keys = false;
            for (unsigned c = 0; c < kControlCount; ++c)
                for (const Binding& b : pl.controls[c]) keys = keys || b.kind != Binding::kPad;
            const bool pad = display_.pad_connected(int(p));
            ImGui::AlignTextToFramePadding();
            if (pad) ImGui::Text("%s%s", display_.player_controller_name(int(p)).c_str(), keys ? " + keyboard" : "");
            else ImGui::TextDisabled(keys ? "The keyboard" : "Not connected");
            ImGui::TableNextColumn();
            ImGui::Checkbox("Rumble", &pl.rumble);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    {
        std::array<std::string, host::kPlayerPlaces> chosen;
        for (unsigned p = 0; p < kPlayers; ++p) chosen[p] = s_.players[p].controller;
        display_.set_player_controllers(chosen);  // "Now" follows the choice at once
    }
    if (ImGui::Button("Reset automatic player order")) display_.reset_player_order();
    ImGui::TextDisabled("Automatic: controllers take the ports in the order each first has a button pressed (%d have), and keep "
                        "them while unplugged. A port with keys is always connected.", display_.joined_pads());
    const auto percent = [](const char* label, unsigned& value, int lo, int hi) {
        int v = int(value);
        if (ImGui::SliderInt(label, &v, lo, hi, "%d%%", ImGuiSliderFlags_AlwaysClamp)) value = unsigned(v);
    };
    percent("Stick dead zone", s_.stick_deadzone, 0, 50);
    percent("Trigger click (where L and R press)", s_.trigger_threshold, 5, 95);
    percent("Stick press threshold (a stick bound to a button)", s_.stick_threshold, 5, 95);
    percent("Rumble strength", s_.rumble_strength, 0, 100);
    display_.set_input_thresholds(s_.stick_threshold, s_.trigger_threshold);

    ImGui::SeparatorText("The GameCube controller's controls");
    for (unsigned p = 0; p < kPlayers; ++p) {
        if (p) ImGui::SameLine();
        char label[16];
        std::snprintf(label, sizeof label, "Player %u", p + 1);
        ImGui::BeginDisabled(capture_ >= 0);
        if (ImGui::RadioButton(label, player_ == p)) player_ = p, note_.clear();
        ImGui::EndDisabled();
    }
    ImGuiIO& io = ImGui::GetIO();
    auto& controls = s_.players[player_].controls;
    if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Pressed by (click one to remove it)", ImGuiTableColumnFlags_WidthStretch, 0.57f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableHeadersRow();
        for (unsigned c = 0; c < kControlCount; ++c) {
            ImGui::PushID(int(c));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(control_label(Control(c)));
            ImGui::TableNextColumn();
            if (capture_ == int(c)) {
                const uint64_t left = capture_until_ > SDL_GetTicks() ? (capture_until_ - SDL_GetTicks() + 999) / 1000 : 0;
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Press a key, a pad's button, stick or trigger, or click... (Esc: cancel, %u s)",
                                   unsigned(left));
            } else {
                auto& list = controls[c];
                if (list.empty()) ImGui::TextDisabled("unused");
                for (size_t i = 0; i < list.size(); ++i) {
                    ImGui::PushID(int(i));
                    if (i) ImGui::SameLine();
                    if (ImGui::SmallButton(binding_label(list[i]).c_str())) {
                        list.erase(list.begin() + std::ptrdiff_t(i));
                        controls_changed_ = true;
                        note_.clear();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(capture_ >= 0);
            if (ImGui::SmallButton("Add")) {
                capture_ = int(c);
                capture_until_ = SDL_GetTicks() + kCaptureMs;
                capture_held_ = display_.buttons();
                note_.clear();
                // The mouse and the pad's navigation are the control's to take until it's set.
                io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableGamepad) | ImGuiConfigFlags_NoMouse;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!note_.empty()) ImGui::TextUnformatted(note_.c_str());
    if (ImGui::Button("Back to the defaults")) {
        // (Keys the first player's defaults name leave whoever had them: a key presses one control.)
        default_controls(s_, player_);
        for (unsigned p = 0; p < kPlayers; ++p)
            for (unsigned c = 0; p != player_ && c < kControlCount; ++c)
                std::erase_if(s_.players[p].controls[c], [&](const Binding& b) {
                    if (b.kind == Binding::kPad) return false;
                    for (unsigned k = 0; k < kControlCount; ++k)
                        if (std::find(controls[k].begin(), controls[k].end(), b) != controls[k].end()) return true;
                    return false;
                });
        controls_changed_ = true;
        note_.clear();
    }
    ImGui::TextWrapped("Pad inputs are the player's own controller's, by an Xbox pad's names; by default its buttons are the GameCube's by "
                       "position (pad X is B, pad B is X), a shoulder button is Z and the triggers are L and R. A stick or trigger on the "
                       "sticks, L or R is analogue; a key or button there presses all the way. A key presses one control.");
    ImGui::TextDisabled("F1 or Esc (controller: Back + Start): this menu. F3: frame times. F9: capture frames for a report. F11 or Alt+Enter: full screen.");
}

void Menu::display_page() {
    ImGui::SeparatorText("Window");
    bool full = display_.fullscreen();
    if (ImGui::Checkbox("Full screen (F11 or Alt+Enter)", &full)) {
        display_.set_fullscreen(full);
        s_.fullscreen = full;
    }
    {
        const std::string current = s_.monitor.empty() ? "The main display" : s_.monitor;
        if (ImGui::BeginCombo("Display", current.c_str())) {
            if (ImGui::Selectable("The main display", s_.monitor.empty())) {
                s_.monitor.clear();
                display_.move_to("");
            }
            const std::vector<std::string> names = display_.monitors();
            for (size_t i = 0; i < names.size(); ++i) {
                ImGui::PushID(int(i));
                if (ImGui::Selectable(names[i].c_str(), names[i] == s_.monitor)) {
                    s_.monitor = names[i];
                    display_.move_to(names[i]);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SeparatorText("Picture");
    char label[96];
    {
        // The picture's shape: the game's cameras would widen to it (enhance.hpp).
        struct Shape {
            unsigned w, h;
            const char* name;
        };
        static constexpr Shape kShapes[] = {
            {0, 0, "Auto (the screen's shape)"}, {4, 3, "4:3 (the original)"}, {16, 10, "16:10"},
            {16, 9, "16:9"}, {64, 27, "21:9 ultrawide"}, {32, 9, "32:9 super ultrawide"},
        };
        const bool can = enhance::aspect_supported();
        std::string current = std::to_string(s_.aspect_w) + ":" + std::to_string(s_.aspect_h);
        for (const Shape& k : kShapes)
            if (k.w == s_.aspect_w && k.h == s_.aspect_h) current = k.name;
        ImGui::BeginDisabled(!can);
        if (ImGui::BeginCombo("Aspect ratio", current.c_str())) {
            for (const Shape& k : kShapes)
                if (ImGui::Selectable(k.name, k.w == s_.aspect_w && k.h == s_.aspect_h)) s_.aspect_w = k.w, s_.aspect_h = k.h;
            ImGui::EndCombo();
        }
        static constexpr const char* kHud[] = {"At the screen's edges", "In the middle 4:3 (as on a 4:3 TV)", "Stretched (as the game draws it)"};
        int hud = int(s_.hud);
        if (ImGui::Combo("HUD (wider than 4:3)", &hud, kHud, 3)) s_.hud = Hud(hud);
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", can ? "(wider: more at the sides; any width:height in mkdd.ini)" : kNeedsAspect);
    }
    {
        // The render resolution, with the size it gives.
        const auto name = [&](unsigned k) {
            if (k == 0) std::snprintf(label, sizeof label, "Auto (the window's size: %ux now)", running_scale_);
            else std::snprintf(label, sizeof label, k == 1 ? "%ux, %ux%u (the original)" : "%ux, %ux%u", k, 640 * k, 480 * k);
            return label;
        };
        const std::string current = name(std::min(s_.scale, 8u));
        if (ImGui::BeginCombo("Render resolution", current.c_str())) {
            for (unsigned k = 0; k <= 8; ++k)
                if (ImGui::Selectable(name(k), s_.scale == k)) s_.scale = k;
            ImGui::EndCombo();
        }
        if (s_.scale != 0 && s_.scale != running_scale_)
            ImGui::TextDisabled("The game is drawn at %ux now: %ux from the next start.", running_scale_, s_.scale);
        else if (s_.scale == 0)
            ImGui::TextDisabled("Auto is taken from the window when the game starts. The GPU's work grows with the square.");
    }
    int mode = int(s_.scaling);
    ImGui::RadioButton("Whole multiples (sharpest)", &mode, int(host::Scaling::integer));
    ImGui::SameLine();
    ImGui::RadioButton("Fit (keeps its shape)", &mode, int(host::Scaling::fit));
    ImGui::SameLine();
    ImGui::RadioButton("Fill the window", &mode, int(host::Scaling::stretch));
    s_.scaling = host::Scaling(mode);
    ImGui::Checkbox("SMAA anti-aliasing", &s_.smaa);
    ImGui::SameLine();
    ImGui::TextDisabled("(smooths jagged edges; with a higher render resolution, smoother still)");
    ImGui::BeginDisabled(!display_.fsr_available());
    ImGui::Checkbox("AMD FSR 1 upscaling", &s_.fsr);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(!display_.fsr_available()                    ? "(this graphics device has no compute shaders)"
                        : s_.scaling == host::Scaling::integer ? "(not at whole multiples)"
                                                               : "(where the picture is shown larger than it's drawn)");
    if (s_.fsr) {
        int sharp = int(s_.sharpness);
        if (ImGui::SliderInt("FSR sharpness", &sharp, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.sharpness = unsigned(sharp);
    }
    int look = int(s_.look);
    ImGui::RadioButton("Plain", &look, int(host::Look::plain));
    ImGui::SameLine();
    ImGui::RadioButton("CRT television", &look, int(host::Look::crt));
    s_.look = host::Look(look);
    if (s_.look == host::Look::crt) {
        int scan = int(s_.scanlines), mask = int(s_.mask);
        if (ImGui::SliderInt("Scanlines", &scan, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.scanlines = unsigned(scan);
        if (ImGui::SliderInt("Phosphor stripes", &mask, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.mask = unsigned(mask);
        ImGui::TextDisabled("The raster is the console's own lines: it shows best at Whole multiples, or a large window.");
    } else {
        ImGui::Checkbox("Smooth (bilinear; off: sharp pixels)", &s_.smooth);
    }
    ImGui::Checkbox("Vsync (off: the lowest latency, may tear)", &s_.vsync);
    {
        static constexpr const char* kRates[] = {"Original (60; 30 with three or four players)", "60", "The display's refresh rate", "Unlimited"};
        const bool can = enhance::frame_rate_supported();
        int rate = int(s_.frame_rate);
        ImGui::BeginDisabled(!can);
        if (ImGui::Combo("Frame rate", &rate, kRates, 4)) s_.frame_rate = FrameRate(rate);
        ImGui::EndDisabled();
        if (!can) ImGui::TextDisabled("%s", kNeedsFrameRate);
    }
    ImGui::BeginDisabled(!display_.gpu().present_wait);
    ImGui::Checkbox("Low latency (with vsync: at most one frame waiting)", &s_.low_latency);
    ImGui::EndDisabled();
    if (!display_.gpu().present_wait) {
        ImGui::SameLine();
        ImGui::TextDisabled("(this graphics device can't wait for a frame to reach the screen)");
    }
    ImGui::Checkbox("Frame times (F3)", &s_.performance_overlay);
    ImGui::SeparatorText("Refresh rate");
    const bool was_match = s_.match_refresh, was_vrr = s_.variable_refresh;
    ImGui::Checkbox("Fullscreen at a multiple of 60 Hz (120 Hz on a 144 Hz monitor)", &s_.match_refresh);
    ImGui::Checkbox("Variable refresh (G-Sync, FreeSync: on in the driver too)", &s_.variable_refresh);
    if ((was_match != s_.match_refresh || was_vrr != s_.variable_refresh) && display_.fullscreen()) {
        display_.set_picture(picture_of(s_));
        display_.set_fullscreen(true);  // the mode for the new choice
    }
    if (!display_.mode_note().empty()) ImGui::TextWrapped("%s", display_.mode_note().c_str());
    if (ImGui::Button("Look at the picture")) peek_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(any key or button comes back)");

    ImGui::SeparatorText("Enhancements");
    static constexpr const char* kPresets[] = {"original", "low-end", "desktop"};
    if (ImGui::BeginCombo("Preset", s_.preset.c_str())) {
        for (const char* p : kPresets)
            if (ImGui::Selectable(p, s_.preset == p)) apply_preset(s_, p);
        ImGui::EndCombo();
    }
    const float limit = display_.gpu().max_anisotropy;
    ImGui::BeginDisabled(limit <= 1.0f);
    static constexpr unsigned kAniso[] = {1, 2, 4, 8, 16};
    std::snprintf(label, sizeof label, s_.anisotropy <= 1 ? "Off (the console's)" : "%ux", s_.anisotropy);
    if (ImGui::BeginCombo("Anisotropic filtering", label)) {
        for (unsigned a : kAniso) {
            if (float(a) > limit && a > 1) continue;
            std::snprintf(label, sizeof label, a == 1 ? "Off (the console's)" : "%ux", a);
            if (ImGui::Selectable(label, s_.anisotropy == a)) s_.anisotropy = a;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (limit <= 1.0f) ImGui::TextDisabled("Anisotropic filtering: this GPU doesn't have it.");
    ImGui::Checkbox("True colour", &s_.true_color);
    ImGui::TextDisabled(s_.true_color ? "24-bit colour: smooth gradients." : "The console's 18-bit colour and its dither pattern, where the game draws that way.");

    ImGui::SeparatorText("Graphics device");
    const std::string chosen = s_.gpu.empty() ? "The best one" : s_.gpu;
    if (ImGui::BeginCombo("Device", chosen.c_str())) {
        if (ImGui::Selectable("The best one", s_.gpu.empty())) s_.gpu.clear();
        for (const std::string& name : display_.gpu().usable)
            if (ImGui::Selectable(name.c_str(), name == s_.gpu)) s_.gpu = name;
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("In use: %s. A change takes effect the next time the game starts.", display_.device_name().c_str());
}

void Menu::game_page() {
    ImGui::SeparatorText("Sound and language");
    int volume = int(s_.volume);
    if (ImGui::SliderInt("Volume", &volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.volume = unsigned(volume);
    ImGui::Checkbox("Mute when unfocused", &s_.mute_unfocused);
    static constexpr const char* kOutputs[] = {"Stereo", "Surround (5.1 from the game's Dolby Surround)", "The device's own (surround where it has six speakers)"};
    int output = int(s_.sound_output);
    if (ImGui::Combo("Sound output (from the next start)", &output, kOutputs, 3)) s_.sound_output = host::SoundOutput(output);
    if (s_.sound_output != host::SoundOutput::stereo)
        ImGui::TextDisabled("For surround, choose Surround in the game's own sound options too.");
    // The playback device: the system's default (followed when it changes: a
    // controller with a headset socket, a streaming app or a monitor can take
    // the default over while the game runs), or one by name.
    if (!sound_devices_loaded_) sound_devices_ = host::AudioOut::playback_devices(), sound_devices_loaded_ = true;
    if (ImGui::BeginCombo("Sound device (from the next start)", s_.sound_device.empty() ? "The system's default" : s_.sound_device.c_str())) {
        if (ImGui::Selectable("The system's default", s_.sound_device.empty())) s_.sound_device.clear();
        for (const std::string& name : sound_devices_)
            if (ImGui::Selectable(name.c_str(), s_.sound_device == name)) s_.sound_device = name;
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Language (from the next start)", "English")) {
        ImGui::Selectable("English", true);  // the only one on the USA disc
        ImGui::EndCombo();
    }

    ImGui::SeparatorText("The GameCube's own settings (read the next time the game starts)");
    static constexpr const char* kSound[] = {"Mono", "Stereo"};
    int sound = std::clamp(int(s_.sound), 0, 1);
    if (ImGui::Combo("Sound", &sound, kSound, 2)) s_.sound = unsigned(sound);
    ImGui::Checkbox("Progressive scan (480p; off: interlaced)", &s_.progressive);

    ImGui::SeparatorText("Save recovery");
    ImGui::TextWrapped("The five latest previous copies of each save are kept. Restore restarts the game; the current save is backed up first.");
    if (ImGui::TreeNode("Previous saves")) {
        if (!history_loaded_ || ImGui::Button("Refresh previous saves")) { save_history_ = save_backups(folder_); history_loaded_ = true; }
        const auto& backups = save_history_;
        if (backups.empty()) ImGui::TextDisabled("No previous saves yet.");
        for (size_t i = 0; i < backups.size(); ++i) {
            ImGui::PushID(int(i));
            ImGui::TextWrapped("%s", backups[i].label.c_str());
            if (ImGui::Button("Restore and restart")) {
                s_.fullscreen = display_.fullscreen();
                if (!save_settings(folder_, s_)) note_ = "Settings could not be saved; restore was not queued.";
                else if (request_save_restore(folder_, backups[i].file, note_)) restart_program();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (!note_.empty()) ImGui::TextWrapped("%s", note_.c_str());

    ImGui::SeparatorText("About");
    ImGui::TextWrapped("Mario Kart: Double Dash!! QilinRecomp %s: a static recompilation, the game's own code compiled for this computer.",
                       MKDD_VERSION);
    ImGui::TextWrapped("Graphics: Vulkan. Window, sound and input: SDL3 (zlib). Menu: Dear ImGui (MIT). SMAA (MIT, Jimenez et al.). "
                       "AMD FidelityFX FSR 1 (MIT).");
    ImGui::TextWrapped("Settings file: %s", settings_file(folder_).string().c_str());
    ImGui::TextWrapped("Saves, captures and the pipeline cache: %s", folder_.string().c_str());
}

}  // namespace mkdd
