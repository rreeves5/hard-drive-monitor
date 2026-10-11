// Toolkit-free logic behind the hdmon-gui desktop app (port of the VB.NET DiskMonitor tray app).
// Everything here is unit tested; gui/*.cpp only draws it.
#pragma once
#include "palette.hpp"
#include <string>
#include <utility>
#include <vector>

#include "history.hpp"

namespace hdmon::gui {

// Worst status over all drives, VB DiskMonitor order: UNKNOWN < OK < WARNING < CRITICAL.
// No drives / only drives without SMART data -> UNKNOWN (gray).
std::string tray_status(const std::vector<Result>& results);
int gui_rank(const std::string& status);
// Strong colour for the tray circle and the soft row tint (RGB hex, same values as the VB app).
std::string status_color(const std::string& status);   // palette::STATUS_* (#00FF00 #FFFF00 #FF0000 #8A8F98)
std::string row_tint(const std::string& status);       // palette::ROW_*    (#00FF00 #FFFF00 #FF0000 #E4E6EA)
std::string status_label(const std::string& status);   // "OK", "Warning", "Critical", "No SMART data"

// "C:" / "E: F:" (Windows letters), "/ /home" (mounts), "Disk 2" (Windows, no letter), "sda".
std::string drive_name(const Drive& d);
std::string health_text(const Assessment& a);           // "93%" or "n/a"
std::string temp_text(const Drive& d);                  // "38 °C" or "—"
std::string size_text(const Drive& d);                  // "2.0 TB" (hdmon's format)

// Drive letters first (sorted), then the rest by device, like the VB scanner.
std::vector<Result> sorted_for_display(std::vector<Result> results);

// Tray tooltip: "Hard Drive Monitor Beta V x" + one "<drive> <health>" line per drive, cut to max_len
// (Windows tray tips hold 127 characters).
std::string tooltip_text(const std::vector<Result>& results, const std::string& version, size_t max_len = 127);
std::string tooltip_scanning(const std::string& version);
std::string tooltip_failed(const std::string& version);

// Two-column Details list (port of VB DriveDetails.Rows): only values the drive reports.
std::vector<std::pair<std::string, std::string>> detail_rows(const Drive& d, const Assessment& a);

// [gui] section of config.toml. Defaults: alert_on_danger = true, scan_interval_minutes = 60 (1..1440).
struct GuiSettings {
    bool alert_on_danger = true;
    int scan_interval_minutes = 60;
    bool auto_check = false;     // [update] auto_check
    bool auto_install = false;   // [update] auto_install
};
GuiSettings load_gui_settings(const json& cfg);
// Writes the four keys back with set_config_value (keeps the rest of the file).
bool save_gui_settings(const std::string& config_path, const GuiSettings& s, std::string* err);

}  // namespace hdmon::gui
