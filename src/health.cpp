#include "health.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace hdmon {

// ---- Health: per-unit multiplicative factors (each unit keeps this fraction of the score)
static const std::vector<std::pair<const char*, double>> HEALTH_FACTORS = {
    {"reallocated_sectors", 0.99},      // ATA 5    -1% per reallocated sector
    {"pending_sectors", 0.97},          // ATA 197  -3% per pending (unstable) sector
    {"offline_uncorrectable", 0.97},    // ATA 198  -3% per offline-uncorrectable sector
    {"reported_uncorrectable", 0.98},   // ATA 187  -2% per reported uncorrectable error
    {"command_timeout", 0.995},         // ATA 188  -0.5% per command timeout
    {"media_errors", 0.97},             // NVMe     -3% per media/data-integrity error
    {"uncorrected_errors", 0.98},       // Windows Storage fallback: uncorrected read+write errors
};
// Wear: score *= (100 - percent_used) / 100
static const int CAP_SMART_FAILED = 5;
static const int CAP_ATTRIBUTE_FAILING_NOW = 10;
static const int CAP_NVME_SPARE_LOW = 10;
static const int CAP_NVME_CRITICAL_WARNING = 20;
static const int CRITICAL_BELOW = 30;
static const int WARNING_BELOW = 70;
// ---- Performance: (points per unit, max points)
static const std::vector<std::tuple<const char*, double, double>> PERF_DEDUCTIONS = {
    {"udma_crc_errors", 1.0, 30}, {"command_timeout", 2.0, 30}, {"pending_sectors", 1.0, 20},
    {"reallocated_sectors", 0.1, 20}, {"offline_uncorrectable", 0.5, 10}, {"media_errors", 1.0, 10}};
static const double PERF_HOT = 10, PERF_VERY_HOT = 25, PERF_THROTTLE_MAX = 15;

struct CwBit { int bit; const char* sev; const char* text; };
static const CwBit NVME_CW_BITS[] = {
    {0, "critical", "available spare space has fallen below the threshold"},
    {1, "warning", "temperature is outside the safe range"},
    {2, "critical", "NVM subsystem reliability is degraded (media or internal errors)"},
    {3, "critical", "drive has switched to READ-ONLY mode"},
    {4, "critical", "volatile memory backup device has failed"},
};

int status_rank(const std::string& s) {
    if (s == "WARNING") return 1;
    if (s == "CRITICAL") return 2;
    return 0;
}

std::vector<std::string> Assessment::reasons() const {
    std::vector<std::string> out;
    for (auto& f : findings) if (f.severity == "critical" || f.severity == "warning") out.push_back(f.message);
    return out;
}
json Assessment::to_json() const {
    json j = json::object();
    j["health"] = health ? json(*health) : json(nullptr);
    j["performance"] = performance ? json(*performance) : json(nullptr);
    j["status"] = status;
    j["reasons"] = reasons();
    json fs = json::array();
    for (auto& f : findings)
        fs.push_back({{"severity", f.severity}, {"message", f.message}, {"metric", f.metric ? json(*f.metric) : json(nullptr)}});
    j["findings"] = fs;
    return j;
}

static std::string plural(int64_t n, const std::string& one, const std::string& many = "") {
    return with_commas(n) + " " + (n == 1 ? one : (many.empty() ? one + "s" : many));
}
static std::string message_for(const std::string& key, int64_t n) {
    if (key == "reallocated_sectors") return plural(n, "reallocated sector") + " - the drive is remapping bad spots to spare area";
    if (key == "pending_sectors") return plural(n, "pending sector") + " - unreadable spots waiting to be remapped; data there may already be lost";
    if (key == "offline_uncorrectable") return plural(n, "offline uncorrectable sector") + " - found by the drive's own surface scan and could not be repaired";
    if (key == "reported_uncorrectable") return plural(n, "reported uncorrectable error") + " - reads failed even after error correction";
    if (key == "command_timeout") return plural(n, "command timeout") + " - the drive stalled on operations (failing drive, cable or power)";
    if (key == "udma_crc_errors") return plural(n, "UDMA CRC error") + " - data got garbled on the cable; reseat or replace the SATA cable/port";
    if (key == "media_errors") return plural(n, "NVMe media error") + " - unrecovered data-integrity errors";
    if (key == "uncorrected_errors") return plural(n, "uncorrected read/write error") + " reported by Windows";
    return plural(n, key);
}
static int64_t critical_count(const std::string& key) {
    if (key == "pending_sectors" || key == "offline_uncorrectable" || key == "reported_uncorrectable" ||
        key == "media_errors" || key == "uncorrected_errors") return 10;
    if (key == "reallocated_sectors") return 100;
    return INT64_MAX;
}

std::pair<int, int> temp_limits(const Drive& d, const json& t) {
    std::string kind = d.kind == "SSD" ? "ssd" : (d.kind == "NVMe" ? "nvme" : "hdd");
    int dw = kind == "hdd" ? 50 : (kind == "ssd" ? 60 : 70);
    int dc = kind == "hdd" ? 55 : (kind == "ssd" ? 70 : 80);
    int warn = static_cast<int>(opt_int(jget(t, (kind + "_temp_warn").c_str())).value_or(dw));
    int crit = static_cast<int>(opt_int(jget(t, (kind + "_temp_crit").c_str())).value_or(dc));
    if (d.temp_limit_warn && *d.temp_limit_warn)
        warn = kind == "hdd" ? std::min<int>(warn, static_cast<int>(*d.temp_limit_warn)) : static_cast<int>(*d.temp_limit_warn);
    if (d.temp_limit_crit && *d.temp_limit_crit) crit = static_cast<int>(*d.temp_limit_crit);
    return {warn, std::max(crit, warn)};
}

Assessment assess(const Drive& d, const json& thresholds) {
    std::vector<Finding> f;
    const json& m = d.metrics;
    auto mi = [&](const char* k) { return opt_int(jget(m, k)); };
    double perf = 100.0;

    auto [tw, tc] = temp_limits(d, thresholds);
    if (d.temperature_c && *d.temperature_c > 0) {
        int64_t t = *d.temperature_c;
        if (t >= tc) {
            f.push_back({"critical", "Temperature " + std::to_string(t) + " C is critical (limit " + std::to_string(tc) + " C) - check cooling", std::string("temperature")});
            perf -= PERF_VERY_HOT;
        } else if (t >= tw) {
            f.push_back({"warning", "Temperature " + std::to_string(t) + " C is high (warning at " + std::to_string(tw) + " C)", std::string("temperature")});
            perf -= PERF_HOT;
        }
    }

    if (!d.smart_available) {
        std::string note = d.notes.empty() ? "device did not report SMART data" : d.notes[0];
        f.push_back({"info", "No SMART data: " + note, std::nullopt});
        return Assessment{std::nullopt, std::nullopt, "UNKNOWN", f};
    }

    double h = 100.0;
    int cap = 100;
    bool force_critical = false, defects = false;

    for (auto& [key, factor] : HEALTH_FACTORS) {
        auto n = mi(key);
        if (!n || *n <= 0) continue;
        h *= std::pow(factor, static_cast<double>(std::min<int64_t>(*n, 100000)));
        defects = true;
        f.push_back({*n >= critical_count(key) ? "critical" : "warning", message_for(key, *n), std::string(key)});
    }
    int64_t crc = mi("udma_crc_errors").value_or(0);
    if (crc > 0) f.push_back({"warning", message_for("udma_crc_errors", crc), std::string("udma_crc_errors")});

    for (auto& [key, per, mx] : PERF_DEDUCTIONS) {
        int64_t n = mi(key).value_or(0);
        if (n > 0) perf -= std::min(mx, per * static_cast<double>(n));
    }

    auto wear = mi("wear_percent_used");
    if (wear) {
        int64_t used = std::max<int64_t>(0, std::min<int64_t>(100, *wear));
        h *= static_cast<double>(100 - used) / 100.0;
        std::string label = d.kind == "NVMe" ? "NVMe" : "SSD";
        std::string w = std::to_string(*wear);
        if (*wear >= 100) f.push_back({"critical", label + " wear: " + w + "% of rated write endurance used - plan a replacement", std::string("wear_percent_used")});
        else if (*wear >= 80) f.push_back({"warning", label + " wear: " + w + "% of rated write endurance used", std::string("wear_percent_used")});
        else if (*wear >= 10) f.push_back({"info", label + " wear: " + w + "% of rated write endurance used", std::string("wear_percent_used")});
    }

    if (d.smart_passed && !*d.smart_passed) {
        cap = std::min(cap, CAP_SMART_FAILED);
        force_critical = true;
        std::string what = starts_with(d.source, "windows") ? "Windows reports this disk as Unhealthy"
                                                           : "Drive's own SMART self-assessment says FAILED";
        f.push_back({"critical", what + " - back up now", std::string("smart_status")});
    }
    if (jget(m, "windows_health_status") == "Warning") {
        defects = true;
        f.push_back({"warning", "Windows reports this disk's health as Warning", std::string("windows_health_status")});
    }
    auto list_of = [&](const char* k) {
        std::vector<std::string> v;
        const json& a = jget(m, k);
        if (a.is_array()) for (auto& x : a) v.push_back(to_str(x));
        return v;
    };
    auto fn = list_of("failing_now");
    if (!fn.empty()) {
        cap = std::min(cap, CAP_ATTRIBUTE_FAILING_NOW);
        force_critical = true;
        f.push_back({"critical", "Attribute below its failure threshold now: " + join(fn, ", "), std::string("failing_now")});
    }
    auto fp = list_of("failed_past");
    if (!fp.empty()) {
        defects = true;
        f.push_back({"warning", "Attribute was below its failure threshold in the past: " + join(fp, ", "), std::string("failed_past")});
    }

    int64_t cw = mi("critical_warning").value_or(0);
    if (cw) {
        for (auto& b : NVME_CW_BITS)
            if (cw & (1LL << b.bit))
                f.push_back({b.sev, "NVMe critical warning (bit " + std::to_string(b.bit) + "): " + b.text, std::string("critical_warning")});
        if (cw & 0b11101) { cap = std::min(cap, CAP_NVME_CRITICAL_WARNING); force_critical = true; }
        else defects = true;
    }

    auto spare = mi("available_spare"), thr = mi("available_spare_threshold");
    if (spare && thr && *thr) {
        std::string s = std::to_string(*spare), t = std::to_string(*thr);
        if (*spare < *thr) {
            cap = std::min(cap, CAP_NVME_SPARE_LOW);
            force_critical = true;
            f.push_back({"critical", "Available spare " + s + "% is below the " + t + "% threshold", std::string("available_spare")});
        } else if (*spare < 100 && *spare <= *thr + 10) {
            defects = true;
            f.push_back({"warning", "Available spare " + s + "% is close to the " + t + "% threshold", std::string("available_spare")});
        } else if (*spare < 100) {
            f.push_back({"info", "Available spare " + s + "% (threshold " + t + "%)", std::string("available_spare")});
        }
    }

    int64_t hot_min = mi("warning_temp_minutes").value_or(0) + mi("critical_temp_minutes").value_or(0);
    if (hot_min > 0) {
        perf -= std::min(PERF_THROTTLE_MAX, 1.0 + static_cast<double>(hot_min) / 60.0);
        f.push_back({"info", "Has spent " + plural(hot_min, "minute") + " above its warning/critical temperature (throttling)", std::string("warning_temp_minutes")});
    }
    int64_t us = mi("unsafe_shutdowns").value_or(0);
    if (us) f.push_back({"info", plural(us, "unsafe shutdown") + " (power lost without a clean shutdown); not a defect by itself", std::string("unsafe_shutdowns")});
    if (d.power_on_hours && *d.power_on_hours >= 43800)
        f.push_back({"info", "Powered on " + with_commas(*d.power_on_hours) + " hours (~" +
                                 fmt_fixed(static_cast<double>(*d.power_on_hours) / 8766.0, 1) + " years)", std::string("power_on_hours")});

    // Python: max(0, min(cap, int(math.floor(round(h, 6)))))
    double h6 = std::nearbyint(h * 1e6) / 1e6;
    int score = std::max(0, std::min(cap, static_cast<int>(std::floor(h6))));
    int perf_i = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(100, py_round(perf))));

    auto any_sev = [&](std::initializer_list<const char*> sevs) {
        for (auto& x : f) for (auto s : sevs) if (x.severity == s) return true;
        return false;
    };
    std::string status = "OK";
    if (force_critical || score < CRITICAL_BELOW) {
        status = "CRITICAL";
        if (score < CRITICAL_BELOW && !force_critical && !any_sev({"critical"}))
            f.push_back({"critical", "Health score " + std::to_string(score) + "% is below " + std::to_string(CRITICAL_BELOW) + "% - back up and replace", std::string("health")});
    } else if (score < WARNING_BELOW || defects) {
        status = "WARNING";
        if (!any_sev({"warning", "critical"})) {
            std::vector<std::string> drivers;
            for (auto& x : f) if (x.metric && (*x.metric == "wear_percent_used" || *x.metric == "available_spare")) drivers.push_back(x.message);
            f.push_back({"warning", "Health score " + std::to_string(score) + "% is below " + std::to_string(WARNING_BELOW) + "%" +
                                        (drivers.empty() ? "" : " - mainly " + join(drivers, "; ")), std::string("health")});
        }
    }
    int worst = 0;
    for (auto& x : f) worst = std::max(worst, x.severity == "critical" ? 2 : (x.severity == "warning" ? 1 : 0));
    const char* by_rank[] = {"OK", "WARNING", "CRITICAL"};
    if (worst > status_rank(status)) status = by_rank[worst];
    auto order = [](const std::string& s) { return s == "critical" ? 0 : (s == "warning" ? 1 : 2); };
    std::stable_sort(f.begin(), f.end(), [&](const Finding& a, const Finding& b) { return order(a.severity) < order(b.severity); });
    return Assessment{score, perf_i, status, f};
}

}  // namespace hdmon
