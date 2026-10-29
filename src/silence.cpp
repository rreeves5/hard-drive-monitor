#include "silence.hpp"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include "config.hpp"
#include "util.hpp"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/time.h>
#endif

namespace hdmon {

int64_t boot_time_epoch() {
#ifdef _WIN32
    return static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(GetTickCount64() / 1000);
#elif defined(__APPLE__)
    struct timeval tv{};
    size_t len = sizeof tv;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &tv, &len, nullptr, 0) == 0) return static_cast<int64_t>(tv.tv_sec);
    return 0;
#else
    std::string stat = read_file("/proc/stat");   // "btime <epoch>" is fixed for the whole boot
    auto p = stat.find("\nbtime ");
    if (p == std::string::npos) return 0;
    return std::strtoll(stat.c_str() + p + 7, nullptr, 10);
#endif
}

std::string silence_marker_path() { return path_join(data_dir(), "silence-until-reboot"); }

bool same_boot(int64_t a, int64_t b) {
    int64_t d = a > b ? a - b : b - a;
    return d <= kSameBootToleranceSeconds;
}

bool silenced_at(const std::string& marker, int64_t boot) {
    if (boot <= 0 || !file_exists(marker)) return false;
    std::string s = trim(read_file(marker));
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 18) return false;
    return same_boot(std::strtoll(s.c_str(), nullptr, 10), boot);
}

bool set_silenced_at(const std::string& marker, bool on, int64_t boot) {
    if (on) {
        if (boot <= 0) return false;
        make_dirs(dirname(marker));
        return write_file(marker, std::to_string(boot) + "\n");
    }
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(marker), ec);
    return !ec;
}

bool silenced() { return silenced_at(silence_marker_path(), boot_time_epoch()); }
bool set_silenced(bool on) { return set_silenced_at(silence_marker_path(), on, boot_time_epoch()); }

}  // namespace hdmon
