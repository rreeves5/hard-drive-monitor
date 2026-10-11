// Health %, Performance % and status (Hard Disk Sentinel style). Constants are documented in README.md.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "smart.hpp"

namespace hdmon {

struct Finding {
    std::string severity;   // critical | warning | info
    std::string message;
    std::optional<std::string> metric;
};

struct Assessment {
    std::optional<int> health;
    std::optional<int> performance;
    std::string status;     // OK | WARNING | CRITICAL | UNKNOWN
    std::vector<Finding> findings;
    std::vector<std::string> reasons() const;
    json to_json() const;
};

int status_rank(const std::string& s);
std::pair<int, int> temp_limits(const Drive& d, const json& thresholds);
Assessment assess(const Drive& d, const json& thresholds = json::object());

}  // namespace hdmon
