// The host Vulkan layer, headless: a frame presented into a 64x36 target at a
// 2:1 aspect must land letterboxed, with black bars above and below.
// Exit 77 (skipped) where there's no Vulkan 1.3 device.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "qilin/host/display.hpp"
#include "qilin/host/vulkan.hpp"

namespace {
bool vulkan_13_device() {
    if (volkInitialize() != VK_SUCCESS || volkGetInstanceVersion() < VK_API_VERSION_1_3) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return false;
    volkLoadInstance(inst);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    bool ok = false;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        ok |= p.apiVersion >= VK_API_VERSION_1_3;
    }
    vkDestroyInstance(inst, nullptr);
    return ok;
}
}  // namespace

int main() {
    if (!vulkan_13_device()) {
        std::puts("vktest: no Vulkan 1.3 device here: skipped");
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;
    }
    qilin::host::Display::Options o;
    o.headless = true;
    o.width = 64;
    o.height = 36;
    qilin::host::Display d(o);
    std::vector<uint8_t> frame(8 * 4 * 4);
    for (size_t i = 0; i < frame.size(); i += 4) frame[i] = 255, frame[i + 1] = 0, frame[i + 2] = 0, frame[i + 3] = 255;  // red
    d.present(8, 4, frame.data(), 2.0f, 4);  // 64x32 inside 64x36: bars of 2 rows
    const std::vector<uint8_t> px = d.read_back();
    auto at = [&](int x, int y) { return &px[static_cast<size_t>(y * 64 + x) * 4]; };
    int failures = 0;
    auto expect = [&](int x, int y, uint8_t r, const char* what) {
        if (at(x, y)[0] != r || at(x, y)[1] != 0 || at(x, y)[2] != 0) {
            std::printf("vktest: %s at (%d,%d) is %u,%u,%u\n", what, x, y, at(x, y)[0], at(x, y)[1], at(x, y)[2]);
            ++failures;
        }
    };
    expect(32, 0, 0, "the top bar");
    expect(32, 35, 0, "the bottom bar");
    expect(32, 18, 255, "the frame");
    expect(0, 2, 255, "the frame's corner");

    // Fill the window: no bars.
    qilin::host::Picture pic;
    pic.scaling = qilin::host::Scaling::stretch;
    d.set_picture(pic);
    d.present(8, 4, frame.data(), 2.0f, 4);
    {
        const std::vector<uint8_t> s = d.read_back();
        if (s[0] != 255 || s[(35 * 64 + 32) * 4] != 255) {
            std::puts("vktest: stretched, the picture doesn't reach the corners");
            ++failures;
        }
    }
    // Whole multiples: a 4-line, 2:1 picture is 8x4; 64x36 holds it 8 times (64x32), so it's
    // 64x32 again; with 6 lines (12x6), 5 times: 60x30, from (2,3).
    pic.scaling = qilin::host::Scaling::integer;
    d.set_picture(pic);
    d.present(8, 4, frame.data(), 2.0f, 6);
    {
        const std::vector<uint8_t> s = d.read_back();
        auto red = [&](int x, int y) { return s[static_cast<size_t>(y * 64 + x) * 4] == 255; };
        if (red(1, 18) || !red(2, 18) || !red(61, 18) || red(62, 18) || red(32, 2) || !red(32, 3) || !red(32, 32) || red(32, 33)) {
            std::puts("vktest: whole multiples: the picture isn't at 60x30 from (2,3)");
            ++failures;
        }
    }
    // A CRT: mid grey on 4 raster lines into 36 rows (9 a line, enough for the
    // gaps to show): a line's centre is bright, the gap between two dark.
    // (Bright lines spread to nearly fill their gaps, as a tube's do.)
    pic.scaling = qilin::host::Scaling::stretch;
    pic.look = qilin::host::Look::crt;
    pic.scanlines = 1.0f;
    pic.mask = 0.0f;
    d.set_picture(pic);
    std::vector<uint8_t> grey(8 * 4 * 4, 128);
    d.present(8, 4, grey.data(), 2.0f, 4);
    {
        const std::vector<uint8_t> s = d.read_back();
        auto g = [&](int y) { return int(s[static_cast<size_t>(y * 64 + 32) * 4 + 1]); };
        // 36 rows, 9 a line: centres at 4.5, 13.5 ...; the gap at 9.
        // crt.frag's beams worked out by hand for grey 128: 148 at a centre, 103 in the gap.
        if (std::abs(g(4) - 148) > 3 || std::abs(g(13) - 148) > 3 || std::abs(g(9) - 103) > 3) {
            std::printf("vktest: CRT: line centre %d, gap %d, next centre %d\n", g(4), g(9), g(13));
            ++failures;
        }
    }
    // A capture: the last picture as it was shown (the CRT's, here), at the target's size.
    {
        const std::vector<uint8_t> shown = d.read_back();
        uint32_t cw = 0, ch = 0;
        const std::vector<uint8_t> shot = d.capture(cw, ch);
        if (cw != 64 || ch != 36 || shot != shown) {
            std::printf("vktest: a capture is %ux%u, %s the picture shown\n", cw, ch, shot == shown ? "and" : "and not");
            ++failures;
        }
    }
    // SMAA: a staircase edge (white where x > y) shown pixel for pixel. Without it every pixel is
    // black or white; with it the steps are blended and the flat areas are untouched.
    pic = qilin::host::Picture{};
    pic.scaling = qilin::host::Scaling::stretch;
    pic.smooth = false;
    std::vector<uint8_t> stairs(64 * 36 * 4, 255);
    for (int y = 0; y < 36; ++y)
        for (int x = 0; x < 64; ++x)
            if (x <= y)
                for (int c = 0; c < 3; ++c) stairs[static_cast<size_t>(y * 64 + x) * 4 + static_cast<size_t>(c)] = 0;
    const auto shades = [&](bool smaa, bool& flat_kept) {
        pic.smaa = smaa;
        d.set_picture(pic);
        d.present(64, 36, stairs.data(), 64.0f / 36.0f, 36);
        const std::vector<uint8_t> s = d.read_back();
        int between = 0;
        for (size_t i = 0; i < s.size(); i += 4) between += s[i] > 8 && s[i] < 247;
        flat_kept = s[(30 * 64 + 2) * 4] == 0 && s[(2 * 64 + 60) * 4] == 255;  // far below and far above the edge
        return between;
    };
    {
        bool flat_plain = false, flat_smaa = false;
        const int plain = shades(false, flat_plain), smoothed = shades(true, flat_smaa);
        if (plain != 0 || smoothed < 20 || !flat_plain || !flat_smaa) {
            std::printf("vktest: SMAA: %d blended pixels without it, %d with it (the flat areas %s)\n", plain, smoothed,
                        flat_plain && flat_smaa ? "kept" : "changed");
            ++failures;
        }
        // And once more without: the pass is really off again.
        if (shades(false, flat_plain) != 0) {
            std::puts("vktest: SMAA stays on after it's switched off");
            ++failures;
        }
    }
    // FSR 1: a 16x9 picture, black on the left half and white on the right, shown four times its
    // size. Flat areas stay as they are, and the edge is sharper than bilinear filtering leaves it.
    if (d.fsr_available()) {
        std::vector<uint8_t> halves(16 * 9 * 4, 255);
        for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 8; ++x)
                for (int c = 0; c < 3; ++c) halves[static_cast<size_t>(y * 16 + x) * 4 + static_cast<size_t>(c)] = 0;
        const auto row = [&](bool fsr) {
            pic = qilin::host::Picture{};
            pic.scaling = qilin::host::Scaling::stretch;
            pic.fsr = fsr;
            pic.sharpness = 0.8f;
            d.set_picture(pic);
            d.present(16, 9, halves.data(), 16.0f / 9.0f, 9);
            const std::vector<uint8_t> s = d.read_back();
            std::vector<int> out(64);
            for (int x = 0; x < 64; ++x) out[static_cast<size_t>(x)] = s[static_cast<size_t>(18 * 64 + x) * 4];
            return out;
        };
        const std::vector<int> soft = row(false), sharp = row(true);
        // Bilinear: 32 and 223 a pixel and a half either side of the edge (x = 30, 33).
        const bool soft_ok = std::abs(soft[30] - 32) <= 3 && std::abs(soft[33] - 223) <= 3;
        const bool flat = sharp[4] == 0 && sharp[20] == 0 && sharp[44] == 255 && sharp[60] == 255;
        const bool sharper = sharp[30] + (255 - sharp[33]) < soft[30] + (255 - soft[33]) - 20;
        if (!soft_ok || !flat || !sharper) {
            std::printf("vktest: FSR: across the edge bilinear gives %d %d %d %d, FSR %d %d %d %d; flat areas %d %d %d %d\n", soft[30], soft[31],
                        soft[32], soft[33], sharp[30], sharp[31], sharp[32], sharp[33], sharp[4], sharp[20], sharp[44], sharp[60]);
            ++failures;
        }
        // At whole multiples FSR stands aside (the pixels are the point): exactly the plain picture.
        pic.scaling = qilin::host::Scaling::integer, pic.smooth = false, pic.fsr = false;
        d.set_picture(pic);
        d.present(16, 9, halves.data(), 16.0f / 9.0f, 9);
        const std::vector<uint8_t> whole = d.read_back();
        pic.fsr = true;
        d.set_picture(pic);
        d.present(16, 9, halves.data(), 16.0f / 9.0f, 9);
        if (d.read_back() != whole) {
            std::puts("vktest: FSR changes the picture at whole multiples");
            ++failures;
        }
        // Both together, then a capture of it: the passes run for a capture as for a present.
        pic.scaling = qilin::host::Scaling::stretch, pic.smaa = true, pic.fsr = true;
        d.set_picture(pic);
        d.present(16, 9, halves.data(), 16.0f / 9.0f, 9);
        const std::vector<uint8_t> both = d.read_back();
        uint32_t cw = 0, ch = 0;
        if (d.capture(cw, ch) != both || both[(18 * 64 + 4) * 4] != 0 || both[(18 * 64 + 60) * 4] != 255) {
            std::puts("vktest: SMAA and FSR together: the capture isn't the picture shown, or the flat areas changed");
            ++failures;
        }
    } else {
        std::puts("vktest: this device's queue has no compute shaders: FSR not tested");
    }
    std::printf("vktest: %s on %s\n", failures ? "FAILED" : "passed", d.device_name().c_str());
    return failures ? 1 : 0;
}
