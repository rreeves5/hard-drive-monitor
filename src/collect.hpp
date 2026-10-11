// Gather drives from the live system (or JSON files) and score them.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "history.hpp"

namespace hdmon {

std::vector<Result> score(const std::vector<Drive>& drives, const json& cfg);
void apply_windows_letters(std::vector<Drive>& drives, const json& inv);
// Throws SmartctlMissing when smartctl is unavailable (and no Windows fallback applies).
std::vector<Result> collect(const json& cfg, const std::vector<std::string>& from_json, json& meta);
std::optional<Result> find_drive(const std::vector<Result>& results, const std::string& dev);

}  // namespace hdmon
