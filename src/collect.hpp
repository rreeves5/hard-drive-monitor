// Gather drives (native SMART first, smartctl as optional fallback) and RAID arrays, and score them.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "history.hpp"
#include "raid.hpp"

namespace hdmon {

struct Scan {
    std::vector<Result> results;     // every physical disk, array members included
    std::vector<RaidArray> arrays;   // members linked to results by drive key
    json meta = json::object();
};

std::vector<Result> score(const std::vector<Drive>& drives, const json& cfg);
void apply_windows_letters(std::vector<Drive>& drives, const json& inv);
// [smart] source: "auto" (native, smartctl fallback when installed), "native" (never smartctl), "smartctl".
std::string smart_source(const json& cfg);
// Throws SmartctlMissing only with source = "smartctl" when smartctl is unavailable.
Scan scan(const json& cfg, const std::vector<std::string>& from_json);
std::vector<Result> collect(const json& cfg, const std::vector<std::string>& from_json, json& meta);
std::optional<Result> find_drive(const std::vector<Result>& results, const std::string& dev);

}  // namespace hdmon
