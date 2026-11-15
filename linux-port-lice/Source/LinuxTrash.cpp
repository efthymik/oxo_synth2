// The Linux "move to Trash" (EditorCore/Trash.hpp contract): the XDG
// Trash spec v1.0 home trash, pure POSIX (no toolkit). Used only in the
// Linux build.
//
// Only the HOME trash is used ($XDG_DATA_HOME/Trash, default
// ~/.local/share/Trash): trashing needs the same filesystem, so anything
// on another device (EXDEV) returns false and the caller deletes the item
// instead — the same contract the Windows port keeps for drives Windows
// cannot recycle (WindowsTrash.cpp).
#include "EditorCore/Trash.hpp"

#include <cstdio>
#include <ctime>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oxo {
namespace {

std::filesystem::path dataHome() {
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg && xdg[0] == '/') return xdg;
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : "") / ".local/share";
}

// Percent-encode everything outside the unreserved set (RFC 2396, the trash
// spec's "URI encoding"); UTF-8 patch names must survive.
std::string uriEscape(const std::string &text) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const unsigned char ch = (unsigned char)c;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '/' || ch == '~' || ch == '!') {
            out.push_back(c);
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 15];
        }
    }
    return out;
}

bool makeDir(const std::filesystem::path &dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    // The trash spec: mode 0700, "create or chmod" for $XDG_DATA_HOME/Trash.
    return ::chmod(dir.c_str(), 0700) == 0;
}

} // namespace

bool MoveToTrash(const std::filesystem::path &path) {
    if (path.empty() || !path.is_absolute()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return false;

    const std::filesystem::path trash = dataHome() / "Trash";
    const std::filesystem::path files = trash / "files", info = trash / "info";
    if (!makeDir(files) || !makeDir(info)) return false;

    // A free name: the original name, then "<name>.2", "<name>.3", …
    const std::string name = path.filename().string();
    std::string slot = name;
    std::error_code slotError;
    for (int n = 2;; ++n) {
        slotError.clear();
        const bool taken = std::filesystem::exists(files / slot, slotError);
        if (!taken && !slotError) break;
        if (n > 999) return false;
        slot = name + "." + std::to_string(n);
    }

    // The .trashinfo record first: the spec requires it beside the entry, and
    // a rename leaves the record ahead of the file for the briefest moment.
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &local);
    const std::string record = "[Trash Info]\nPath=" + uriEscape(path.string()) + "\nDeletionDate=" + stamp + "\n";
    const std::filesystem::path recordPath = info / (slot + ".trashinfo");
    const std::filesystem::path recordTmp = info / (slot + ".trashinfo.tmp");
    {
        const int fd = ::open(recordTmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return false;
        const ssize_t wrote = ::write(fd, record.data(), record.size());
        ::close(fd);
        if (wrote != (ssize_t)record.size()) {
            ::unlink(recordTmp.c_str());
            return false;
        }
    }
    if (::rename(recordTmp.c_str(), recordPath.c_str()) != 0) {
        ::unlink(recordTmp.c_str());
        return false;
    }

    // The move: a failure here (another device, a race on the name) leaves
    // only a stray record behind — harmless, remove it and give up.
    if (::rename(path.c_str(), (files / slot).c_str()) != 0) {
        ::unlink(recordPath.c_str());
        return false;
    }
    return true;
}
} // namespace oxo
