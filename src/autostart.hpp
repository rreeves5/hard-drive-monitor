// "Start at login" for hdmon-gui: Linux ~/.config/autostart/*.desktop, macOS LaunchAgent,
// Windows Task Scheduler logon task (elevated runs; the GUI needs admin like the VB app) or HKCU Run.
#pragma once
#include <string>
#include <vector>

#include "util.hpp"

namespace hdmon {

extern const char* AUTOSTART_LABEL;       // com.rreeves5.hard-drive-monitor.gui (LaunchAgent label)
extern const char* AUTOSTART_TASK;        // "Hard Drive Monitor GUI" (Windows task / Run value name)

// ---- pure generators (unit tested)
std::string desktop_exec_quote(const std::string& arg);   // freedesktop Exec= quoting (incl. %% and \\)
// Linux .desktop. autostart=true adds X-GNOME-Autostart-enabled; false gives the menu launcher.
std::string desktop_entry(const std::vector<std::string>& argv, const std::string& icon, bool autostart);
std::string launch_agent_plist(const std::vector<std::string>& argv);
// schtasks /Create arguments for a logon task with highest privileges.
std::vector<std::string> windows_autostart_task_args(const std::string& exe);   // schtasks fallback
// Task Scheduler XML for the elevated logon task registered through the COM API.
std::string windows_autostart_task_xml(const std::string& exe, const std::string& user);
std::string windows_run_value(const std::string& exe);    // "\"C:\\...\\hdmon-gui.exe\""
std::string autostart_path(OS os);                         // file used on Linux/macOS ("" on Windows)

// ---- system
bool autostart_enabled();
bool set_autostart(bool enable, const std::string& exe, std::string* err);

}  // namespace hdmon
