// Windows: with the game's manifest (mkdd/res), the narrow APIs are UTF-8, so a
// folder named in any script works through fopen, std::filesystem's string()
// (which otherwise throws for a character outside the code page) and getenv.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

int main() {
    namespace fs = std::filesystem;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++failures;
    };
    check(GetACP() == CP_UTF8, "the process's code page is UTF-8");
    // Cyrillic, CJK and an emoji: outside every legacy code page at once.
    const fs::path folder = fs::temp_directory_path() / fs::path(u8"mkdd-Дмитрий-測試-\U0001F3CE");
    std::error_code ec;
    fs::create_directories(folder, ec);
    check(!ec, "a folder with such a name is made");
    std::string narrow;
    try {
        narrow = (folder / "mkdd.log").string();
        check(true, "path::string() doesn't throw");
    } catch (...) {
        check(false, "path::string() doesn't throw");
    }
    if (std::FILE* f = std::fopen(narrow.c_str(), "w")) {
        std::fputs("written\n", f);
        std::fclose(f);
    }
    check(fs::exists(folder / "mkdd.log", ec), "fopen with the narrow (UTF-8) name writes the file there");
    _putenv_s("MKDD_TEST_FOLDER", narrow.c_str());
    const char* back = std::getenv("MKDD_TEST_FOLDER");
    check(back && fs::path(reinterpret_cast<const char8_t*>(back)) == folder / "mkdd.log", "the environment carries it as UTF-8");
    fs::remove_all(folder, ec);
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
