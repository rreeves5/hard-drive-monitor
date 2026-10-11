#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#define ISATTY _isatty
#define FILENO _fileno
#else
#include <unistd.h>
#define ISATTY isatty
#define FILENO fileno
#endif

namespace hdmon {

static const std::map<std::string, std::string> COLORS = {
    {"red", "\033[31m"}, {"green", "\033[32m"}, {"yellow", "\033[33m"}, {"blue", "\033[34m"},
    {"cyan", "\033[36m"}, {"dim", "\033[2m"}, {"bold", "\033[1m"}, {"bred", "\033[1;31m"}};
static const char* RESET = "\033[0m";
static std::string status_color(const std::string& s) {
    if (s == "OK") return "green";
    if (s == "WARNING") return "yellow";
    if (s == "CRITICAL") return "bred";
    return "dim";
}
static std::string sev_color(const std::string& s) {
    return s == "critical" ? "red" : (s == "warning" ? "yellow" : "dim");
}
static const char* SPARK[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

Painter::Painter(const std::string& mode) {
    if (mode == "always") on = true;
    else if (mode == "never" || !get_env("NO_COLOR").empty()) on = false;
    else on = ISATTY(FILENO(stdout)) != 0;
    console_init(on);
}
std::string Painter::operator()(const std::string& text, const std::string& color) const {
    if (!on || color.empty()) return text;
    return COLORS.at(color) + text + RESET;
}

std::string fmt_size(std::optional<int64_t> b) {
    if (!b || *b == 0) return "-";
    const std::pair<const char*, double> units[] = {{"TB", 1e12}, {"GB", 1e9}, {"MB", 1e6}};
    for (auto& [unit, f] : units) {
        if (static_cast<double>(*b) >= f) {
            double v = static_cast<double>(*b) / f;
            return (v < 100 ? fmt_fixed(v, 1) : fmt_fixed(v, 0)) + " " + unit;
        }
    }
    return std::to_string(*b) + " B";
}
std::string fmt_hours(std::optional<int64_t> h) {
    if (!h) return "-";
    if (*h < 8766) return with_commas(*h) + " h";
    return with_commas(*h) + " h (" + fmt_fixed(static_cast<double>(*h) / 8766.0, 1) + "y)";
}
std::string pct_color(std::optional<int> v) {
    if (!v) return "dim";
    return *v >= 70 ? "green" : (*v >= 30 ? "yellow" : "red");
}
std::string bar(std::optional<int> v, int width) {
    if (!v) return std::string(static_cast<size_t>(width), ' ');
    int64_t n = py_round(static_cast<double>(*v) / 100.0 * width);
    std::string out;
    for (int i = 0; i < width; ++i) out += i < n ? "█" : "░";
    return out;
}
std::string sparkline(const std::vector<std::optional<double>>& values, double lo, double hi) {
    std::string out;
    for (auto& v : values) {
        if (!v) { out += " "; continue; }
        double r = hi == lo ? 0 : (*v - lo) / (hi - lo);
        int64_t i = py_round(r * 7);
        out += SPARK[std::max<int64_t>(0, std::min<int64_t>(7, i))];
    }
    return out;
}

std::string table(const std::vector<std::string>& headers, const std::vector<std::vector<Cell>>& rows,
                  const Painter& paint, const std::vector<int>& right) {
    std::vector<size_t> widths;
    for (auto& h : headers) widths.push_back(u8len(h));
    for (auto& r : rows)
        for (size_t i = 0; i < r.size(); ++i) widths[i] = std::max(widths[i], u8len(r[i].first));
    auto is_right = [&](size_t i) { return std::find(right.begin(), right.end(), static_cast<int>(i)) != right.end(); };
    std::vector<std::string> lines, parts;
    for (size_t i = 0; i < headers.size(); ++i)
        parts.push_back(paint(is_right(i) ? rjust(headers[i], widths[i]) : ljust(headers[i], widths[i]), "bold"));
    lines.push_back(rstrip(join(parts, "  ")));
    parts.clear();
    for (auto w : widths) { std::string s; for (size_t k = 0; k < w; ++k) s += "─"; parts.push_back(s); }
    lines.push_back(join(parts, "  "));
    for (auto& r : rows) {
        parts.clear();
        for (size_t i = 0; i < r.size(); ++i)
            parts.push_back(paint(is_right(i) ? rjust(r[i].first, widths[i]) : ljust(r[i].first, widths[i]), r[i].second));
        lines.push_back(rstrip(join(parts, "  ")));
    }
    return join(lines, "\n");
}

std::string device_label(const Drive& d) {
    if (d.volumes.empty()) return d.device;
    std::vector<std::string> v(d.volumes.begin(), d.volumes.begin() + std::min<size_t>(3, d.volumes.size()));
    return d.device + " (" + join(v, " ") + (d.volumes.size() > 3 ? "…" : "") + ")";
}

std::string status_view(std::vector<Result> results, const Painter& paint, const json& meta, const Changes& changes) {
    if (results.empty()) return "No drives found.";
    std::stable_sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
        bool pa = starts_with(a.first.device, "PhysicalDrive"), pb = starts_with(b.first.device, "PhysicalDrive");
        if (pa != pb) return pa < pb;
        return a.first.device < b.first.device;
    });
    std::vector<std::string> headers = {"DEVICE", "MODEL", "SERIAL", "TYPE", "SIZE", "TEMP", "POWER-ON", "CYCLES", "HEALTH", "PERF", "STATUS"};
    std::vector<std::vector<Cell>> rows;
    for (auto& [d, a] : results) {
        std::string model = d.model.value_or("-");
        if (model.empty()) model = "-";
        if (u8len(model) > 26) model = u8substr(model, 25) + "…";
        std::string temp_c;
        for (auto& f : a.findings) if (f.metric && *f.metric == "temperature") temp_c = sev_color(f.severity);
        std::string health = a.health ? bar(a.health, 8) + " " + rjust(std::to_string(*a.health), 3) + "%" : "n/a";
        rows.push_back({
            {device_label(d), "cyan"}, {model, ""}, {d.serial && !d.serial->empty() ? *d.serial : "-", "dim"}, {d.kind, ""},
            {fmt_size(d.capacity_bytes), ""},
            {d.temperature_c ? std::to_string(*d.temperature_c) + " C" : "-", temp_c},
            {fmt_hours(d.power_on_hours), ""},
            {d.power_cycles ? with_commas(*d.power_cycles) : "-", ""},
            {health, pct_color(a.health)},
            {a.performance ? std::to_string(*a.performance) + "%" : "n/a", pct_color(a.performance)},
            {a.status, status_color(a.status)},
        });
    }
    std::vector<std::string> out = {table(headers, rows, paint, {4, 5, 6, 7, 9})};
    std::vector<std::string> details;
    for (auto& [d, a] : results) {
        std::vector<Finding> items;
        for (auto& f : a.findings) if (f.severity != "info") items.push_back(f);
        if (items.empty() && a.status == "UNKNOWN")
            for (auto& f : a.findings) if (starts_with(f.message, "No SMART")) items.push_back(f);
        std::vector<json> ch;
        auto it = changes.find(d.key());
        if (it != changes.end()) for (auto& c : it->second) if (truthy(jget(c, "alert"))) ch.push_back(c);
        if (items.empty() && ch.empty()) continue;
        details.push_back(paint(d.short_name(), "cyan") + paint("  " + a.status, status_color(a.status)));
        for (auto& c : ch)
            details.push_back("   " + paint("▲ " + to_str(c["metric"]) + ": " + to_str(c["old"]) + " -> " + to_str(c["new"]) + " since last run", "red"));
        for (auto& f : items) {
            std::string mark = f.severity == "critical" ? "✖" : (f.severity == "warning" ? "!" : "·");
            details.push_back("   " + paint(mark, sev_color(f.severity)) + " " + f.message);
        }
    }
    if (!details.empty()) { out.push_back(""); out.insert(out.end(), details.begin(), details.end()); }
    std::vector<std::string> summary;
    for (const char* s : {"OK", "WARNING", "CRITICAL", "UNKNOWN"}) {
        int n = 0;
        for (auto& r : results) if (r.second.status == s) ++n;
        if (n) summary.push_back(paint(std::to_string(n) + " " + s, status_color(s)));
    }
    out.push_back("");
    out.push_back(std::to_string(results.size()) + " drive(s): " + join(summary, "  "));
    if (auto w = jstr(meta, "warning")) out.push_back(paint(*w, "yellow"));
    return join(out, "\n");
}

std::string detail_view(const Drive& d, const Assessment& a, const Painter& paint) {
    std::vector<std::string> L;
    L.push_back(paint(d.device + "  " + d.model.value_or(""), "bold"));
    auto o = [](const std::optional<std::string>& s) { return s.value_or(""); };
    std::string protocol = d.dev_type ? (d.protocol && !d.protocol->empty() ? *d.protocol : "-") + " (smartctl -d " + *d.dev_type + ")" : o(d.protocol);
    std::string verdict = d.smart_passed ? (*d.smart_passed ? "PASSED" : "FAILED") : "-";
    std::vector<std::pair<std::string, std::string>> kv = {
        {"Serial", o(d.serial)}, {"Firmware", o(d.firmware)}, {"Type", d.kind}, {"Protocol", protocol},
        {"Capacity", fmt_size(d.capacity_bytes)},
        {"Rotation", d.rotation_rate && *d.rotation_rate ? std::to_string(*d.rotation_rate) + " rpm" : ""},
        {"Form factor", o(d.form_factor)}, {"Interface", o(d.interface)},
        {"Volumes", join(d.volumes, " ")},
        {"Temperature", d.temperature_c ? std::to_string(*d.temperature_c) + " C" : ""},
        {"Power-on", d.power_on_hours ? fmt_hours(d.power_on_hours) : ""},
        {"Power cycles", d.power_cycles ? with_commas(*d.power_cycles) : ""},
        {"SMART verdict", verdict}, {"Source", d.source}};
    for (auto& [k, v] : kv) if (!v.empty()) L.push_back("  " + ljust(k + ":", 15) + v);
    L.push_back("");
    std::string h = a.health ? std::to_string(*a.health) + "%" : "n/a";
    std::string p = a.performance ? std::to_string(*a.performance) + "%" : "n/a";
    L.push_back("  Health      " + paint(bar(a.health, 20), pct_color(a.health)) + " " + paint(h, pct_color(a.health)));
    L.push_back("  Performance " + paint(bar(a.performance, 20), pct_color(a.performance)) + " " + paint(p, pct_color(a.performance)));
    L.push_back("  Status      " + paint(a.status, status_color(a.status)));
    if (!a.findings.empty()) {
        L.push_back("");
        for (auto& f : a.findings) L.push_back("  " + paint(ljust(upper(f.severity), 9), sev_color(f.severity)) + f.message);
    }
    if (!d.ata_attributes.empty()) {
        L.push_back("");
        L.push_back(paint("  ATA SMART attributes", "bold"));
        std::vector<std::vector<Cell>> rows;
        for (auto& at : d.ata_attributes) {
            int64_t id = jint(at, "id").value_or(0);
            bool key_id = id == 5 || id == 187 || id == 188 || id == 197 || id == 198 || id == 199;
            int64_t raw_v = opt_int(jget(at, "raw_value")).value_or(0);
            std::string failing = jstr(at, "when_failed").value_or("");
            std::string col = failing == "now" ? "red" : ((key_id && raw_v) || !failing.empty() ? "yellow" : "");
            std::string name = jget(at, "name").is_null() ? "" : to_str(at["name"]);
            rows.push_back({{std::to_string(id), col}, {name, col}, {to_str(jget(at, "value")), ""},
                            {to_str(jget(at, "worst")), ""}, {to_str(jget(at, "thresh")), ""},
                            {truthy(jget(at, "prefailure")) ? "Pre-fail" : "Old_age", "dim"},
                            {failing.empty() ? "-" : failing, failing.empty() ? "dim" : "red"},
                            {to_str(jget(at, "raw")), col}});
        }
        std::string t = table({"ID", "ATTRIBUTE", "VALUE", "WORST", "THRESH", "TYPE", "FAILED", "RAW"}, rows, paint, {0, 2, 3, 4});
        for (auto& line : split(t, '\n')) L.push_back("  " + line);
    }
    if (!d.nvme_log.empty()) {
        L.push_back("");
        L.push_back(paint("  NVMe SMART / health log", "bold"));
        for (auto it = d.nvme_log.begin(); it != d.nvme_log.end(); ++it) {
            const json& v = it.value();
            if (v.is_object() || v.is_array()) continue;
            const std::string& k = it.key();
            bool hi = k == "critical_warning" || k == "media_errors" || k == "percentage_used" || k == "available_spare" || k == "unsafe_shutdowns";
            std::string col = ((k == "critical_warning" || k == "media_errors") && truthy(v)) ? "red" : "";
            L.push_back("    " + ljust(k + (hi ? "  *" : ""), 34) + paint(to_str(v), col));
        }
    }
    if (!d.windows_reliability.empty()) {
        L.push_back("");
        L.push_back(paint("  Windows storage reliability counters", "bold"));
        for (auto it = d.windows_reliability.begin(); it != d.windows_reliability.end(); ++it)
            L.push_back("    " + ljust(it.key(), 28) + to_str(it.value()));
    }
    if (!d.self_test.empty()) {
        const json& st = d.self_test;
        L.push_back("");
        L.push_back(paint("  Self-test", "bold"));
        std::string cur = jstr(st, "status").value_or("");
        std::string line = "    Current: " + (cur.empty() ? std::string("-") : cur);
        if (truthy(jget(st, "in_progress"))) line += " (in progress, " + to_str(jget(st, "remaining_percent")) + "% remaining)";
        L.push_back(line);
        size_t n = 0;
        for (auto& row : d.self_test_log) {
            if (n++ >= 5) break;
            const json& lba = jget(row, "lba");
            std::string lbas = (!lba.is_null() && !(lba.is_string() && lba.get<std::string>() == "-")) ? "  LBA " + to_str(lba) : "";
            L.push_back("    " + ljust(to_str(jget(row, "type")), 16) + " " +
                        paint(to_str(jget(row, "status")), truthy(jget(row, "passed")) ? "green" : "red") +
                        "  at " + to_str(jget(row, "lifetime_hours")) + " h" + lbas);
        }
    }
    if (!d.notes.empty()) {
        L.push_back("");
        for (auto& n : d.notes) L.push_back(paint("  note: " + n, "dim"));
    }
    return join(L, "\n");
}

std::string history_view(const json& summary, const Painter& paint) {
    if (!summary.is_array() || summary.empty())
        return "No history yet. Run `hdmon status` or `hdmon check` to record a run.";
    std::vector<std::string> L;
    for (auto& drv : summary) {
        const json& rs = drv["readings"];
        const json& last = rs.back();
        std::optional<int> lh = jint(last, "health") ? std::optional<int>(static_cast<int>(*jint(last, "health"))) : std::nullopt;
        L.push_back(paint(jstr(drv, "device").value_or("None") + "  " + jstr(drv, "model").value_or("") + "  [" +
                          (jstr(drv, "serial") && !jstr(drv, "serial")->empty() ? *jstr(drv, "serial") : "-") + "]", "bold"));
        L.push_back("  " + to_str(drv["n"]) + " readings, " + local_ts(jstr(drv, "first_ts").value_or("")) + " -> " +
                    local_ts(jstr(drv, "last_ts").value_or("")));
        std::vector<std::optional<double>> hv;
        std::vector<std::string> hs;
        for (auto& r : rs) {
            auto h = jint(r, "health");
            hv.push_back(h ? std::optional<double>(static_cast<double>(*h)) : std::nullopt);
        }
        size_t start = rs.size() > 4 ? rs.size() - 4 : 0;
        for (size_t i = start; i < rs.size(); ++i) if (auto h = jint(rs[i], "health")) hs.push_back(std::to_string(*h));
        std::string lstat = jstr(last, "status").value_or("");
        L.push_back("  Health   " + paint(sparkline(hv), pct_color(lh)) + "  " + (hs.empty() ? "n/a" : join(hs, " -> ")) +
                    "%  now " + paint(lstat, status_color(lstat)));
        std::vector<std::optional<double>> temps;
        std::vector<int64_t> tv;
        for (auto& r : rs) {
            auto t = jint(r, "temperature");
            temps.push_back(t ? std::optional<double>(static_cast<double>(*t)) : std::nullopt);
            if (t) tv.push_back(*t);
        }
        if (!tv.empty()) {
            auto [mn, mx] = std::minmax_element(tv.begin(), tv.end());
            L.push_back("  Temp     " + sparkline(temps, static_cast<double>(*mn - 1), static_cast<double>(*mx + 1)) + "  " +
                        std::to_string(*mn) + "-" + std::to_string(*mx) + " C");
        }
        const json& csl = drv["changes_since_last"];
        if (!csl.empty()) {
            L.push_back(paint("  Worse since the previous run:", "red"));
            for (auto& c : csl) L.push_back("    ▲ " + to_str(c["metric"]) + ": " + to_str(c["old"]) + " -> " + to_str(c["new"]));
        }
        std::vector<json> flagged;
        for (auto& c : drv["changes_in_window"]) {
            std::string k = jstr(c, "kind").value_or("");
            if (k == "status" || k == "counter" || k == "health") flagged.push_back(c);
        }
        if (!flagged.empty()) {
            L.push_back("  Changes in this window:");
            size_t s = flagged.size() > 10 ? flagged.size() - 10 : 0;
            for (size_t i = s; i < flagged.size(); ++i)
                L.push_back("    " + local_ts(jstr(flagged[i], "ts").value_or("")) + "  " + to_str(flagged[i]["metric"]) + ": " +
                            to_str(flagged[i]["old"]) + " -> " + to_str(flagged[i]["new"]));
        } else if (csl.empty()) {
            L.push_back(paint("  No attribute got worse in this window.", "green"));
        }
        L.push_back("");
    }
    return rstrip(join(L, "\n"));
}

std::string selftest_view(const Drive& d, const Painter& paint) {
    std::vector<std::string> L;
    const json& st = d.self_test;
    L.push_back(d.device + "  " + d.model.value_or(""));
    std::string cur = jstr(st, "status").value_or("");
    std::string line = "  Current status: " + (cur.empty() ? std::string("-") : cur);
    if (truthy(jget(st, "in_progress"))) line += "  (" + to_str(jget(st, "remaining_percent")) + "% remaining)";
    L.push_back(line);
    auto sm = jint(st, "short_minutes"), em = jint(st, "extended_minutes");
    if ((sm && *sm) || (em && *em))
        L.push_back("  Typical duration: short " + (sm && *sm ? std::to_string(*sm) : "?") + " min, long " +
                    (em && *em ? std::to_string(*em) : "?") + " min");
    if (d.self_test_log.empty()) L.push_back("  Self-test log is empty.");
    for (auto& row : d.self_test_log) {
        const json& lba = jget(row, "lba");
        std::string lbas = (!lba.is_null() && !(lba.is_string() && lba.get<std::string>() == "-")) ? "  first error LBA " + to_str(lba) : "";
        L.push_back("  " + ljust(to_str(jget(row, "type")), 18) +
                    paint(ljust(to_str(jget(row, "status")), 40), truthy(jget(row, "passed")) ? "green" : "red") +
                    "  at " + to_str(jget(row, "lifetime_hours")) + " h" + lbas);
    }
    return join(L, "\n");
}

}  // namespace hdmon
