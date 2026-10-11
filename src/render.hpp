// Terminal rendering (colored tables, detail view, history).
#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "history.hpp"

namespace hdmon {

class Painter {
public:
    explicit Painter(const std::string& mode = "auto");   // auto | always | never
    std::string operator()(const std::string& text, const std::string& color) const;
    bool on = false;
};

using Cell = std::pair<std::string, std::string>;   // text, color ("" = none)

std::string fmt_size(std::optional<int64_t> b);
std::string fmt_hours(std::optional<int64_t> h);
std::string pct_color(std::optional<int> v);
std::string bar(std::optional<int> v, int width = 10);
std::string sparkline(const std::vector<std::optional<double>>& values, double lo = 0, double hi = 100);
std::string table(const std::vector<std::string>& headers, const std::vector<std::vector<Cell>>& rows,
                  const Painter& paint, const std::vector<int>& right = {});
std::string device_label(const Drive& d);
std::string status_view(std::vector<Result> results, const Painter& paint, const json& meta = json::object(),
                        const Changes& changes = {});
std::string detail_view(const Drive& d, const Assessment& a, const Painter& paint);
std::string history_view(const json& summary, const Painter& paint);
std::string selftest_view(const Drive& d, const Painter& paint);

}  // namespace hdmon
