// Native OS services used instead of helper programs (see docs/shell-outs.md).
// Every function returns false with a reason so the caller can fall back to the helper program.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace hdmon::osn {

// Desktop notification. Windows: Shell_NotifyIcon balloon (a toast on Windows 10/11).
// macOS: UNUserNotificationCenter when running inside an app bundle. Linux: D-Bus org.freedesktop.Notifications.
bool notify(const std::string& title, const std::string& body, std::string& how_or_err);

// Windows Task Scheduler 2.0 COM API (ITaskService) in the root folder. Stubs return false elsewhere.
bool task_register_xml(const std::string& name, const std::string& xml, std::string& err);
bool task_delete(const std::string& name, std::string& err);
int task_exists(const std::string& name);   // 1 yes, 0 no, -1 could not ask

// macOS launchd, per-user domain, through ServiceManagement (SMJobSubmit / SMJobRemove / SMJobCopyDictionary).
// The plist in ~/Library/LaunchAgents makes the job permanent; these only load / unload it in the running session.
bool launchd_submit_plist(const std::string& plist_path, std::string& err);
bool launchd_remove(const std::string& label, std::string& err);
int launchd_loaded(const std::string& label);   // 1 yes, 0 no, -1 could not ask

}  // namespace hdmon::osn
