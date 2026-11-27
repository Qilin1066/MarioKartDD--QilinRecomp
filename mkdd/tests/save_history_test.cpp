#include "save_history.hpp"
#include <chrono>
#include <fstream>
#include <iterator>
#include <cstdio>
namespace fs = std::filesystem;
int main() {
    const fs::path folder = fs::temp_directory_path() / ("mkdd-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path save = folder / "card-a" / "GM4E01-save.gci";
    fs::create_directories(save.parent_path());
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok" : "FAIL", what); if (!ok) ++failures; };
    const auto write = [&](const std::string& text) { std::ofstream out(save, std::ios::binary); out << text; };
    const auto read = [&](const fs::path& path) { std::ifstream in(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); };
    std::string error;
    for (int i = 0; i < 8; ++i) {
        write("version " + std::to_string(i));
        check(mkdd::backup_save(folder, save, error), "backup succeeds");
    }
    auto backups = mkdd::save_backups(folder);
    check(backups.size() == 5, "history bounded to five per save");
    const auto selected = backups.back().file; // oldest retained: exercises pruning during restore
    const std::string selected_data = read(selected);
    write("current save");
    check(mkdd::request_save_restore(folder, selected, error), "restore request queued");
    check(read(save) == "current save", "queued restore leaves running game's save untouched");
    check(mkdd::apply_save_restore(folder, error) && read(save) == selected_data, "restore applied before next boot");
    bool current_preserved = false;
    for (const auto& item : mkdd::save_backups(folder)) current_preserved |= read(item.file) == "current save";
    check(current_preserved, "pre-restore save can be recovered");
    check(!fs::exists(folder / "restore-save.txt"), "completed restore request removed");
    check(!mkdd::request_save_restore(folder, folder / "../outside.bak", error), "outside path rejected");
    { std::ofstream invalid(folder / "restore-save.txt"); invalid << "../../outside.bak\n"; }
    check(!mkdd::apply_save_restore(folder, error) && read(save) == selected_data, "bad request cannot modify current save");
    check(!fs::exists(folder / "restore-save.txt") && fs::exists(folder / "restore-save.failed.txt") &&
              mkdd::apply_save_restore(folder, error),
          "a failed request is set aside: the next start isn't stopped by it again");
    // A request edited on Windows: a byte-order mark and CR LF.
    write("current again");
    const auto second = mkdd::save_backups(folder).front().file;
    const std::string second_data = read(second);
    check(mkdd::request_save_restore(folder, second, error), "second restore request queued");
    {
        const std::string line = read(folder / "restore-save.txt");
        std::ofstream edited(folder / "restore-save.txt", std::ios::binary);
        edited << "\xEF\xBB\xBF" << line.substr(0, line.size() - 1) << "\r\n";
    }
    check(mkdd::apply_save_restore(folder, error) && read(save) == second_data, "request with a byte-order mark and CR LF applied");
    { std::ofstream huge(folder / "restore-save.txt", std::ios::binary); huge << std::string(2000, 'a') << '\n'; }
    check(!mkdd::apply_save_restore(folder, error), "oversized request refused");
    fs::remove(folder / "restore-save.txt");
    // A stale staging file from an interrupted operation is never offered as a backup.
    { std::ofstream partial(backups.front().file.string() + ".tmp"); partial << "partial"; }
    check(mkdd::save_backups(folder).size() == 5, "incomplete backups ignored");
    fs::create_directories(folder / "save-backups/card-a/blocked.gci.history");
    const fs::path blocked = folder / "card-a" / "blocked.gci";
    fs::create_directory(blocked);
    check(!mkdd::backup_save(folder, blocked, error), "non-file destination reports failure");
    // Only the memory cards' files have a history, and only they can be restored over.
    { std::ofstream settings(folder / "settings.ini"); settings << "x"; }
    check(!mkdd::backup_save(folder, folder / "settings.ini", error), "a file outside the cards is refused");
    fs::create_directories(folder / "save-backups/elsewhere/file.history");
    { std::ofstream stray(folder / "save-backups/elsewhere/file.history/00000000000000000001-000000.bak"); stray << "x"; }
    check(mkdd::save_backups(folder).size() == 5, "a backup that would restore outside the cards is not offered");
    check(!mkdd::request_save_restore(folder, folder / "save-backups/elsewhere/file.history/00000000000000000001-000000.bak", error),
          "nor accepted");
    check(mkdd::save_backups(folder).front().label.starts_with("Card A: GM4E01-save / "), "labels name the card and the file");
    fs::remove_all(folder);
    return failures ? 1 : 0;
}
