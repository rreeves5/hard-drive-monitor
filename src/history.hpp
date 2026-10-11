// SQLite run log and change detection.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "health.hpp"

struct sqlite3;

namespace hdmon {

using Result = std::pair<Drive, Assessment>;
using Changes = std::map<std::string, json>;   // drive key -> array of change objects

// Changes since the previous reading. kind: status | counter | health | trend.
json compare_readings(const json& prev, const std::string& status, std::optional<int> health, const json& metrics);

class History {
public:
    explicit History(const std::string& path);
    ~History();
    History(const History&) = delete;
    History& operator=(const History&) = delete;
    json last_reading(const std::string& key);
    Changes record(const std::vector<Result>& results, const std::string& source = "live", const std::string& ts = "");
    json drives();
    json readings(const std::string& key, int limit = 50);
    json summary(int limit = 20, const std::string& drive_filter = "");
private:
    sqlite3* db_ = nullptr;
    void exec(const char* sql);
};

}  // namespace hdmon
