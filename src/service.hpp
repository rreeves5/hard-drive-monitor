// Scheduling: systemd user timer / cron (Linux), launchd agent (macOS), Task Scheduler (Windows).
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "util.hpp"

namespace hdmon {

using Out = std::function<void(const std::string&)>;
extern const char* SERVICE_LABEL;   // launchd label
extern const char* TASK_NAME;       // Windows task / systemd unit stem
extern const char* CRON_MARK;

int64_t parse_interval(const std::string& text);   // throws std::invalid_argument
std::vector<std::string> program_args(std::optional<OS> os = std::nullopt);
std::map<std::string, std::string> systemd_units(const std::vector<std::string>& argv, int64_t interval_s);
std::vector<std::string> cron_lines(const std::vector<std::string>& argv, int64_t interval_s);
std::string launchd_plist(const std::vector<std::string>& argv, int64_t interval_s, const std::string& log_dir);
std::string windows_task_xml(const std::vector<std::string>& argv, int64_t interval_s, bool highest, const std::string& user);
std::string utf16le_with_bom(const std::string& utf8);

int install_service(int64_t interval_s, bool enable, bool system, bool highest, bool dry_run, const Out& out);
int uninstall_service(bool system, const Out& out);

}  // namespace hdmon
