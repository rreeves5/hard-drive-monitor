// Desktop notifications (native, helper programs as fallback) and e-mail (via the curl command-line tool).
// Alerts fire only on worsening.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "history.hpp"

namespace hdmon {

struct Alert { std::string severity, title, body; };

std::string drive_label(const Drive& d);
std::vector<Alert> build_alerts(const std::vector<Result>& results, const Changes& changes);
// RAID: degraded / failed / rebuild started / rebuild complete (array_changes from History::record).
std::vector<Alert> build_array_alerts(const std::vector<RaidArray>& arrays, const std::vector<Result>& results,
                                      const Changes& array_changes);
std::pair<bool, std::string> notify_desktop(const std::string& title, const std::string& body);
std::pair<bool, std::string> send_email(const json& cfg, const std::string& subject, const std::string& body);
// Minimal alerts: title "Hard Drive Monitor", body one line per drive ("C: NX-2TB, Warning, 88%").
std::string alert_line(const Drive& d, const Assessment& a);
std::string alert_text(const std::vector<Alert>& alerts);   // the lines joined with "\n"
// [alerts] min_level = "warning" (default) or "critical" (drops everything below CRITICAL, incl. "rebuild complete").
std::vector<Alert> filter_alerts(const json& cfg, std::vector<Alert> alerts);
std::vector<std::string> dispatch(const json& cfg, const std::vector<Alert>& alerts);
// The RFC 5322 message and the curl config (`curl -K -`) used for SMTP; exposed for tests.
std::string build_email_message(const json& cfg, const std::string& subject, const std::string& body, const std::string& host);
std::string build_curl_config(const json& cfg, const std::string& message_path, const std::string& password);

}  // namespace hdmon
