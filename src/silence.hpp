// "Silence until next reboot": no desktop pop-ups (GUI red alert window, tray balloon, scheduled `hdmon check`
// notification) until the computer restarts. Scans, history and e-mail keep running.
// A small marker file holds the boot time it was set in; restarting the app keeps it, a reboot makes it stale.
#pragma once
#include <cstdint>
#include <string>

namespace hdmon {

// Boot times computed from uptime drift a few seconds between reads; within this they are the same boot.
constexpr int64_t kSameBootToleranceSeconds = 120;

int64_t boot_time_epoch();                 // when this OS session started (Unix seconds); 0 if unknown
std::string silence_marker_path();         // <data_dir>/silence-until-reboot
bool same_boot(int64_t a, int64_t b);

// Testable core: explicit marker path and boot time.
bool silenced_at(const std::string& marker, int64_t boot);
bool set_silenced_at(const std::string& marker, bool on, int64_t boot);   // false if the file could not be written/removed

// Convenience: default marker path and the real boot time.
bool silenced();
bool set_silenced(bool on);

}  // namespace hdmon
