// Desktop notifications and e-mail (via the curl command-line tool). Alerts fire only on worsening.
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
std::vector<std::string> dispatch(const json& cfg, const std::vector<Alert>& alerts);
// The RFC 5322 message and the curl config (`curl -K -`) used for SMTP; exposed for tests.
std::string build_email_message(const json& cfg, const std::string& subject, const std::string& body, const std::string& host);
std::string build_curl_config(const json& cfg, const std::string& message_path, const std::string& password);

}  // namespace hdmon
